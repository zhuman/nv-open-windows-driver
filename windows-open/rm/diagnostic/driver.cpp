#include <ntddk.h>

#include "nvplatform_mmio.h"
#include "nvplatform_resources.h"
#include "nvplatform_system.h"

using NV_STATUS = LONG;

extern "C" NV_STATUS coreInitializeRm(void);
extern "C" void coreShutdownRm(void);

namespace {

constexpr USHORT NvidiaVendorId = 0x10DE;
constexpr SIZE_T PmcBoot0Offset = 0x00000000;
constexpr SIZE_T PmcBoot42Offset = 0x00000A00;
constexpr SIZE_T RequiredIdentityLength = PmcBoot42Offset + sizeof(ULONG);

constexpr ULONG RemoveLockTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('R') << 16) |
    (static_cast<ULONG>('d') << 24);

struct DeviceExtension
{
    PDEVICE_OBJECT lowerDevice;
    IO_REMOVE_LOCK removeLock;
    NV_PLATFORM_TRANSLATED_RESOURCES resources;
    NV_PLATFORM_MMIO_MAPPING registerMapping;
    ULONG boot0;
    ULONG boot42;
    BOOLEAN started;
};

DeviceExtension* GetExtension(_In_ PDEVICE_OBJECT deviceObject) noexcept
{
    return static_cast<DeviceExtension*>(deviceObject->DeviceExtension);
}

NTSTATUS CompleteIrp(_Inout_ PIRP irp, _In_ NTSTATUS status) noexcept
{
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS ReleaseRemoveLockCompletion(
    _In_ PDEVICE_OBJECT,
    _In_ PIRP irp,
    _In_ void* context) noexcept
{
    IoReleaseRemoveLock(static_cast<PIO_REMOVE_LOCK>(context), irp);
    return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS ForwardWithRemoveLock(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp,
    _In_ BOOLEAN powerIrp) noexcept
{
    DeviceExtension* extension = GetExtension(deviceObject);
    const NTSTATUS status = IoAcquireRemoveLock(&extension->removeLock, irp);
    if (!NT_SUCCESS(status))
    {
        return CompleteIrp(irp, status);
    }

    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(
        irp,
        ReleaseRemoveLockCompletion,
        &extension->removeLock,
        TRUE,
        TRUE,
        TRUE);
    return powerIrp
        ? PoCallDriver(extension->lowerDevice, irp)
        : IoCallDriver(extension->lowerDevice, irp);
}

NTSTATUS StartCompletion(
    _In_ PDEVICE_OBJECT,
    _In_ PIRP irp,
    _In_ void* context) noexcept
{
    if (irp->PendingReturned)
    {
        KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS SynchronousRequestCompletion(
    _In_ PDEVICE_OBJECT,
    _In_ PIRP irp,
    _In_ void* context) noexcept
{
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS ReadPciConfiguration(
    _In_ PDEVICE_OBJECT lowerDevice,
    _Out_ PCI_COMMON_CONFIG* configuration) noexcept
{
    RtlZeroMemory(configuration, sizeof(*configuration));

    PIRP request = IoAllocateIrp(lowerDevice->StackSize, FALSE);
    if (request == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    request->IoStatus.Status = STATUS_NOT_SUPPORTED;

    IO_STACK_LOCATION* stack = IoGetNextIrpStackLocation(request);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_READ_CONFIG;
    stack->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
    stack->Parameters.ReadWriteConfig.Buffer = configuration;
    stack->Parameters.ReadWriteConfig.Offset = 0;
    stack->Parameters.ReadWriteConfig.Length = PCI_COMMON_HDR_LENGTH;
    IoSetCompletionRoutine(
        request,
        SynchronousRequestCompletion,
        &event,
        TRUE,
        TRUE,
        TRUE);

    NTSTATUS status = IoCallDriver(lowerDevice, request);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
    }
    status = request->IoStatus.Status;
    IoFreeIrp(request);
    return status;
}

void ResetDeviceHardware(_Inout_ DeviceExtension* extension) noexcept
{
    extension->started = FALSE;
    NvPlatformUnmapMmio(&extension->registerMapping);
    extension->boot0 = 0;
    extension->boot42 = 0;
    RtlZeroMemory(&extension->resources, sizeof(extension->resources));
}

NTSTATUS ValidateRegisterAperture(_Inout_ DeviceExtension* extension) noexcept
{
    PCI_COMMON_CONFIG configuration = {};
    NTSTATUS status = ReadPciConfiguration(
        extension->lowerDevice,
        &configuration);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (configuration.VendorID == 0 || configuration.VendorID == PCI_INVALID_VENDORID)
    {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    if (configuration.VendorID != NvidiaVendorId)
    {
        return STATUS_NOT_SUPPORTED;
    }

    const ULONG barLow = configuration.u.type0.BaseAddresses[0];
    if (barLow == 0 || (barLow & PCI_ADDRESS_IO_SPACE) != 0)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ULONGLONG registerBarAddress = barLow & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    if ((barLow & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT)
    {
        registerBarAddress |= static_cast<ULONGLONG>(
            configuration.u.type0.BaseAddresses[1]) << 32;
    }
    if (registerBarAddress > MAXLONGLONG)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    const NV_PLATFORM_MEMORY_RESOURCE* registerRange = nullptr;
    for (ULONG index = 0; index < extension->resources.memoryCount; ++index)
    {
        const NV_PLATFORM_MEMORY_RESOURCE& range = extension->resources.memory[index];
        if (range.physicalAddress.QuadPart ==
            static_cast<LONGLONG>(registerBarAddress))
        {
            registerRange = &range;
            break;
        }
    }
    if (registerRange == nullptr || registerRange->length < RequiredIdentityLength)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    status = NvPlatformMapMmio(
        registerRange->physicalAddress,
        RequiredIdentityLength,
        &extension->registerMapping);
    if (NT_SUCCESS(status))
    {
        status = NvPlatformReadMmio32(
            &extension->registerMapping,
            PmcBoot0Offset,
            &extension->boot0);
    }
    if (NT_SUCCESS(status))
    {
        status = NvPlatformReadMmio32(
            &extension->registerMapping,
            PmcBoot42Offset,
            &extension->boot42);
    }
    if (!NT_SUCCESS(status) ||
        (extension->boot0 == MAXULONG && extension->boot42 == MAXULONG))
    {
        ResetDeviceHardware(extension);
        return NT_SUCCESS(status) ? STATUS_DEVICE_DOES_NOT_EXIST : status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS ForwardStartSynchronously(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp) noexcept
{
    DeviceExtension* extension = GetExtension(deviceObject);
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, StartCompletion, &event, TRUE, TRUE, TRUE);

    NTSTATUS status = IoCallDriver(extension->lowerDevice, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = irp->IoStatus.Status;
    }
    return status;
}

NTSTATUS StartDevice(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp,
    _In_ const IO_STACK_LOCATION& stack) noexcept
{
    DeviceExtension* extension = GetExtension(deviceObject);
    ResetDeviceHardware(extension);
    NTSTATUS status = IoAcquireRemoveLock(&extension->removeLock, irp);
    if (!NT_SUCCESS(status))
    {
        return CompleteIrp(irp, status);
    }

    status = ForwardStartSynchronously(deviceObject, irp);
    if (NT_SUCCESS(status))
    {
        status = NvPlatformDecodeTranslatedResources(
            stack.Parameters.StartDevice.AllocatedResourcesTranslated,
            &extension->resources);
        if (NT_SUCCESS(status) && extension->resources.memoryCount == 0)
        {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (NT_SUCCESS(status))
        {
            status = ValidateRegisterAperture(extension);
        }
    }

    if (NT_SUCCESS(status))
    {
        extension->started = TRUE;
        DbgPrintEx(
            DPFLTR_IHVVIDEO_ID,
            DPFLTR_INFO_LEVEL,
            "NVRM diagnostic: start has %lu memory and %lu interrupt resources, boot0=%08lx boot42=%08lx\n",
            extension->resources.memoryCount,
            extension->resources.interruptCount,
            extension->boot0,
            extension->boot42);
    }
    else
    {
        ResetDeviceHardware(extension);
    }

    IoReleaseRemoveLock(&extension->removeLock, irp);
    return CompleteIrp(irp, status);
}

NTSTATUS RemoveDevice(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp) noexcept
{
    DeviceExtension* extension = GetExtension(deviceObject);
    ResetDeviceHardware(extension);

    IoReleaseRemoveLockAndWait(&extension->removeLock, irp);
    IoSkipCurrentIrpStackLocation(irp);
    const NTSTATUS status = IoCallDriver(extension->lowerDevice, irp);
    IoDetachDevice(extension->lowerDevice);
    IoDeleteDevice(deviceObject);
    return status;
}

NTSTATUS DispatchPassThrough(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp) noexcept
{
    return ForwardWithRemoveLock(deviceObject, irp, FALSE);
}

NTSTATUS DispatchPower(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp) noexcept
{
    return ForwardWithRemoveLock(deviceObject, irp, TRUE);
}

NTSTATUS DispatchPnp(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp) noexcept
{
    const IO_STACK_LOCATION* stack = IoGetCurrentIrpStackLocation(irp);
    switch (stack->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        return StartDevice(deviceObject, irp, *stack);

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    {
        DeviceExtension* extension = GetExtension(deviceObject);
        ResetDeviceHardware(extension);
        return ForwardWithRemoveLock(deviceObject, irp, FALSE);
    }

    case IRP_MN_REMOVE_DEVICE:
    {
        DeviceExtension* extension = GetExtension(deviceObject);
        const NTSTATUS status = IoAcquireRemoveLock(&extension->removeLock, irp);
        if (!NT_SUCCESS(status))
        {
            return CompleteIrp(irp, status);
        }
        return RemoveDevice(deviceObject, irp);
    }

    default:
        return ForwardWithRemoveLock(deviceObject, irp, FALSE);
    }
}

NTSTATUS AddDevice(
    _In_ PDRIVER_OBJECT driverObject,
    _In_ PDEVICE_OBJECT physicalDeviceObject) noexcept
{
    PDEVICE_OBJECT deviceObject = nullptr;
    NTSTATUS status = IoCreateDevice(
        driverObject,
        sizeof(DeviceExtension),
        nullptr,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    DeviceExtension* extension = GetExtension(deviceObject);
    RtlZeroMemory(extension, sizeof(*extension));
    IoInitializeRemoveLock(&extension->removeLock, RemoveLockTag, 0, 0);

    status = IoAttachDeviceToDeviceStackSafe(
        deviceObject,
        physicalDeviceObject,
        &extension->lowerDevice);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(deviceObject);
        return status;
    }

    deviceObject->Flags |= extension->lowerDevice->Flags &
        (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

void DriverUnload(_In_ PDRIVER_OBJECT) noexcept
{
    coreShutdownRm();
    NvPlatformSystemShutdown();
}

} // namespace

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT driverObject,
    _In_ PUNICODE_STRING registryPath) noexcept
{
    UNREFERENCED_PARAMETER(registryPath);

    NTSTATUS status = NvPlatformSystemInitialize(driverObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    const NV_STATUS rmStatus = coreInitializeRm();
    if (rmStatus != 0)
    {
        NvPlatformSystemShutdown();
        return STATUS_UNSUCCESSFUL;
    }

    for (ULONG major = 0; major <= IRP_MJ_MAXIMUM_FUNCTION; ++major)
    {
        driverObject->MajorFunction[major] = DispatchPassThrough;
    }
    driverObject->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    driverObject->MajorFunction[IRP_MJ_POWER] = DispatchPower;
    driverObject->DriverExtension->AddDevice = AddDevice;
    driverObject->DriverUnload = DriverUnload;
    return STATUS_SUCCESS;
}