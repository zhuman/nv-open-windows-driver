#include <ntddk.h>

#include "nvplatform_resources.h"
#include "nvplatform_system.h"

using NV_STATUS = LONG;

extern "C" NV_STATUS coreInitializeRm(void);
extern "C" void coreShutdownRm(void);

namespace {

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
    }

    if (NT_SUCCESS(status))
    {
        extension->started = TRUE;
        DbgPrintEx(
            DPFLTR_IHVVIDEO_ID,
            DPFLTR_INFO_LEVEL,
            "NVRM diagnostic: start has %lu memory and %lu interrupt resources\n",
            extension->resources.memoryCount,
            extension->resources.interruptCount);
    }
    else
    {
        RtlZeroMemory(&extension->resources, sizeof(extension->resources));
    }

    IoReleaseRemoveLock(&extension->removeLock, irp);
    return CompleteIrp(irp, status);
}

NTSTATUS RemoveDevice(
    _In_ PDEVICE_OBJECT deviceObject,
    _Inout_ PIRP irp) noexcept
{
    DeviceExtension* extension = GetExtension(deviceObject);
    extension->started = FALSE;
    RtlZeroMemory(&extension->resources, sizeof(extension->resources));

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
        extension->started = FALSE;
        RtlZeroMemory(&extension->resources, sizeof(extension->resources));
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