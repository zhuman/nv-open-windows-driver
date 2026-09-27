#include "device.hpp"

#if defined(NV_RM_USER_LINK_PROBE)
#include "rmosxfac.h"
#endif

#include <new>

namespace nv::iddcx {

namespace {

NTSTATUS ReadPciConfig(
    void* context,
    void* buffer,
    ULONG offset,
    ULONG length,
    ULONG* bytesRead)
{
    const NTSTATUS status = static_cast<ConfigSpace*>(context)->Read(
        offset, buffer, length);
    *bytesRead = NT_SUCCESS(status) ? length : 0;
    return status;
}

NTSTATUS WritePciConfig(
    void* context,
    const void* buffer,
    ULONG offset,
    ULONG length,
    ULONG* bytesWritten)
{
    const NTSTATUS status = static_cast<ConfigSpace*>(context)->Write(
        offset, buffer, length);
    *bytesWritten = NT_SUCCESS(status) ? length : 0;
    return status;
}

NV_STATUS AllocateDmaBuffer(
    void* context,
    NvU64 size,
    NvU64 alignment,
    void** providerAllocation,
    void** address,
    NvU64* logicalAddress)
{
    if (context == nullptr || size == 0 || size > SIZE_MAX ||
        alignment == 0 || alignment > MAXDWORD ||
        providerAllocation == nullptr || address == nullptr ||
        logicalAddress == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    WDF_COMMON_BUFFER_CONFIG configuration;
    WDF_COMMON_BUFFER_CONFIG_INIT(
        &configuration,
        static_cast<ULONG>(alignment));
    WDFCOMMONBUFFER commonBuffer = nullptr;
    const NTSTATUS status = WdfCommonBufferCreateWithConfig(
        static_cast<DmaEngine*>(context)->Handle(),
        static_cast<SIZE_T>(size),
        &configuration,
        WDF_NO_OBJECT_ATTRIBUTES,
        &commonBuffer);
    if (!NT_SUCCESS(status))
    {
        return status == STATUS_INSUFFICIENT_RESOURCES
            ? NV_ERR_NO_MEMORY
            : NV_ERR_OPERATING_SYSTEM;
    }

    *providerAllocation = commonBuffer;
    *address = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer);
    *logicalAddress = static_cast<NvU64>(
        WdfCommonBufferGetAlignedLogicalAddress(commonBuffer).QuadPart);
    return NV_OK;
}

void FreeDmaBuffer(void*, void* providerAllocation)
{
    if (providerAllocation != nullptr)
    {
        WdfObjectDelete(static_cast<WDFCOMMONBUFFER>(providerAllocation));
    }
}

NTSTATUS CreateRmGpuContext(
    _Inout_ DeviceContext* context,
    _In_ WDFCMRESLIST resourcesTranslated) noexcept
{
    const PciIdentity* identity = context->configSpace.Identity();
    if (identity == nullptr)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    NV_PLATFORM_RM_GPU_ATTACH_INFO attachInfo = {};
    attachInfo.domain = identity->domain;
    attachInfo.bus = identity->bus;
    attachInfo.device = identity->device;
    attachInfo.function = identity->function;

    const ULONG resourceCount = WdfCmResourceListGetCount(resourcesTranslated);
    const ULONG barCount = context->configSpace.MemoryBarCount();
    for (ULONG barIndex = 0; barIndex < barCount; ++barIndex)
    {
        const PHYSICAL_ADDRESS barAddress =
            context->configSpace.MemoryBarAddress(barIndex);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR barRange = nullptr;
        for (ULONG resourceIndex = 0; resourceIndex < resourceCount; ++resourceIndex)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR candidate =
                WdfCmResourceListGetDescriptor(resourcesTranslated, resourceIndex);
            if (candidate != nullptr &&
                candidate->Type == CmResourceTypeMemory &&
                candidate->u.Memory.Start.QuadPart == barAddress.QuadPart)
            {
                barRange = candidate;
                break;
            }
        }

        if (barRange == nullptr)
        {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        attachInfo.apertures[barIndex].physicalAddress =
            static_cast<NvU64>(barRange->u.Memory.Start.QuadPart);
        attachInfo.apertures[barIndex].length = barRange->u.Memory.Length;
    }

    attachInfo.apertureCount = static_cast<NvU8>(barCount);
    attachInfo.apertures[0].kernelAddress = context->registerAperture.Address();
    attachInfo.interruptLine = context->interrupt.Vector();
    attachInfo.interruptCount = context->interrupt.Count();

    const NV_STATUS status = NvPlatformRmGpuContextCreate(
        context,
        &attachInfo,
        &context->rmGpuContext);
    return status == NV_OK
        ? STATUS_SUCCESS
        : status == NV_ERR_NO_MEMORY
            ? STATUS_INSUFFICIENT_RESOURCES
            : STATUS_DRIVER_INTERNAL_ERROR;
}

void DestroyRmGpuContext(_Inout_ DeviceContext* context) noexcept
{
    NvPlatformRmGpuMarkShutdown(context->rmGpuContext);
    NvPlatformRmGpuContextDestroy(context->rmGpuContext);
    context->rmGpuContext = nullptr;
}

} // namespace

_Use_decl_annotations_
NTSTATUS DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    IDD_CX_CLIENT_CONFIG iddConfiguration;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfiguration);
    iddConfiguration.EvtIddCxAdapterInitFinished = AdapterInitFinished;
    iddConfiguration.EvtIddCxAdapterCommitModes = AdapterCommitModes;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfiguration);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = PrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = EnterD0;
    pnpCallbacks.EvtDeviceD0Exit = ExitD0;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = DeviceCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    void* contextStorage = GetDeviceContext(device);
    auto* context = new (contextStorage) DeviceContext{};

    const NV_STATUS nvStatus = nvCommonHostInitialize(
        &context->commonHost,
        GetCommonHostServices());
    if (nvStatus != NV_OK)
    {
        WdfObjectDelete(device);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    const NV_STATUS nvPortStatus = nvPortWindowsInitialize(&context->commonHost);
    if (nvPortStatus != NV_OK)
    {
        nvCommonHostShutdown(&context->commonHost);
        WdfObjectDelete(device);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    context->nvPortInitialized = true;

    if (!NT_SUCCESS(NvPlatformPciInitialize()))
    {
        nvPortWindowsShutdown(&context->commonHost);
        context->nvPortInitialized = false;
        nvCommonHostShutdown(&context->commonHost);
        WdfObjectDelete(device);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

#if defined(NV_RM_USER_LINK_PROBE)
    if (!RmInitRm())
    {
        nvPortWindowsShutdown(&context->commonHost);
        context->nvPortInitialized = false;
        nvCommonHostShutdown(&context->commonHost);
        WdfObjectDelete(device);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    RmDestroyRm();
#endif

    return IddCxDeviceInitialize(device);
}

_Use_decl_annotations_
void DeviceCleanup(WDFOBJECT deviceObject)
{
    DeviceContext* context = GetDeviceContext(deviceObject);
    context->adapter.Reset();
    if (context->nvPortInitialized)
    {
        nvPortWindowsShutdown(&context->commonHost);
        context->nvPortInitialized = false;
    }
    NvPlatformPciShutdown();
    nvCommonHostShutdown(&context->commonHost);
    context->~DeviceContext();
}

_Use_decl_annotations_
NTSTATUS PrepareHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesRaw,
    WDFCMRESLIST resourcesTranslated)
{
    DeviceContext* context = GetDeviceContext(device);
    const ULONG resourceCount = WdfCmResourceListGetCount(resourcesTranslated);

    if (resourceCount == 0)
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS status = context->configSpace.Initialize(device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    const PciIdentity* identity = context->configSpace.Identity();
    if (identity == nullptr)
    {
        context->configSpace.Reset();
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    const NV_PLATFORM_PCI_LOCATION location{
        identity->domain,
        identity->bus,
        identity->device,
        identity->function,
    };
    const NV_PLATFORM_PCI_CONFIG_ACCESS access{
        &context->configSpace,
        ReadPciConfig,
        WritePciConfig,
    };
    status = NvPlatformRegisterPciConfig(
        &location, &access, &context->pciConfigRegistration);
    if (!NT_SUCCESS(status))
    {
        context->configSpace.Reset();
        return status;
    }

    status = context->registerAperture.Initialize(
        device,
        resourcesTranslated,
        context->configSpace);
    if (!NT_SUCCESS(status))
    {
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    status = NvPlatformRegisterMmioAperture(
        static_cast<ULONGLONG>(
            context->configSpace.RegisterBarAddress().QuadPart),
        context->registerAperture.Address(),
        context->registerAperture.Length(),
        &context->mmioRegistration);
    if (!NT_SUCCESS(status))
    {
        context->registerAperture.Reset();
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    status = context->dmaEngine.Initialize(device);
    if (!NT_SUCCESS(status))
    {
        NvPlatformUnregisterMmioAperture(context->mmioRegistration);
        context->mmioRegistration = nullptr;
        context->registerAperture.Reset();
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    const NV_PLATFORM_DMA_PROVIDER dmaProvider{
        &context->dmaEngine,
        AllocateDmaBuffer,
        FreeDmaBuffer,
    };
    const NV_STATUS dmaStatus = NvPlatformRegisterDmaProvider(
        &dmaProvider, &context->dmaRegistration);
    if (dmaStatus != NV_OK)
    {
        context->dmaEngine.Reset();
        NvPlatformUnregisterMmioAperture(context->mmioRegistration);
        context->mmioRegistration = nullptr;
        context->registerAperture.Reset();
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    status = context->interrupt.Initialize(
        device,
        resourcesRaw,
        resourcesTranslated);
    if (!NT_SUCCESS(status))
    {
        NvPlatformUnregisterDmaProvider(context->dmaRegistration);
        context->dmaRegistration = nullptr;
        context->dmaEngine.Reset();
        NvPlatformUnregisterMmioAperture(context->mmioRegistration);
        context->mmioRegistration = nullptr;
        context->registerAperture.Reset();
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    status = CreateRmGpuContext(context, resourcesTranslated);
    if (!NT_SUCCESS(status))
    {
        context->interrupt.Reset();
        NvPlatformUnregisterDmaProvider(context->dmaRegistration);
        context->dmaRegistration = nullptr;
        context->dmaEngine.Reset();
        NvPlatformUnregisterMmioAperture(context->mmioRegistration);
        context->mmioRegistration = nullptr;
        context->registerAperture.Reset();
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ReleaseHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesTranslated);

    DeviceContext* context = GetDeviceContext(device);
    DestroyRmGpuContext(context);
    context->interrupt.Reset();
    NvPlatformUnregisterDmaProvider(context->dmaRegistration);
    context->dmaRegistration = nullptr;
    context->dmaEngine.Reset();
    NvPlatformUnregisterMmioAperture(context->mmioRegistration);
    context->mmioRegistration = nullptr;
    context->registerAperture.Reset();
    NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
    context->pciConfigRegistration = nullptr;
    context->configSpace.Reset();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EnterD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    UNREFERENCED_PARAMETER(previousState);
    return GetDeviceContext(device)->adapter.EnterD0(device);
}

_Use_decl_annotations_
NTSTATUS ExitD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE targetState)
{
    UNREFERENCED_PARAMETER(targetState);
    GetDeviceContext(device)->adapter.ExitD0();
    return STATUS_SUCCESS;
}

} // namespace nv::iddcx