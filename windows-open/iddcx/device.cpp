#include "device.hpp"
#include "display_modes.hpp"
#include "driver_parameters.hpp"
#include "output.hpp"
#include "trace.h"

#if defined(NV_RM_USER_LINK_PROBE)
#include "nvrm_gpu_attach.h"
#include "nvplatform_mmio_rm.h"
#include "nvkms_windows.h"
#include "rmosxfac.h"
#endif

#include <new>

#include "device.tmh"

extern "C" void NvKmsWindowsTraceMessage(const char* message)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%s", message);
}

extern "C" void NvKmsWindowsTraceStateInitEngine(
    unsigned int index,
    unsigned int descriptor,
    const char* name,
    int begin)
{
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "RM state init engine: phase=%s index=%u descriptor=0x%08X name=%s",
        begin ? "begin" : "end",
        index,
        descriptor,
        name != nullptr ? name : "<unknown>");
}

extern "C" void NvKmsWindowsTraceStateLoadEngine(
    unsigned int index,
    unsigned int descriptor,
    const char* name,
    int begin)
{
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "RM state load engine: phase=%s index=%u descriptor=0x%08X name=%s",
        begin ? "begin" : "end",
        index,
        descriptor,
        name != nullptr ? name : "<unknown>");
}

extern "C" void NvKmsWindowsTraceStatePostLoadEngine(
    unsigned int index,
    unsigned int descriptor,
    const char* name,
    int begin)
{
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "RM state post-load engine: phase=%s index=%u descriptor=0x%08X name=%s",
        begin ? "begin" : "end",
        index,
        descriptor,
        name != nullptr ? name : "<unknown>");
}

extern "C" void NvKmsWindowsTraceAssert(
    unsigned int lineNumber,
    unsigned long long returnAddress)
{
    HMODULE module = nullptr;
    unsigned long long callerOffset = returnAddress;

    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &module))
    {
        callerOffset -= reinterpret_cast<unsigned long long>(module);
    }

    TraceEvents(
        TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "RM assertion: line=%u caller=0x%llX",
        lineNumber,
        callerOffset);
}

namespace nv::iddcx {

namespace {

#if defined(NV_RM_USER_LINK_PROBE)
constexpr wchar_t GspFirmwareDirectory[] = L"ProgramData";
constexpr wchar_t GspFirmwareSuffix[] = L"\\nv-iddcx\\gsp_ga10x.bin";

NV_STATUS InitializeRmGpuFirmware(
    _In_ NV_PLATFORM_RM_GPU_CONTEXT* gpuContext) noexcept
{
    wchar_t firmwarePath[MAX_PATH] = {};
    const DWORD directoryLength = GetEnvironmentVariableW(
        GspFirmwareDirectory,
        firmwarePath,
        ARRAYSIZE(firmwarePath));
    if (directoryLength == 0 ||
        directoryLength >= ARRAYSIZE(firmwarePath) ||
        wcscat_s(firmwarePath, GspFirmwareSuffix) != 0)
    {
        return NV_ERR_INVALID_PATH;
    }

    const HANDLE firmwareFile = CreateFile2(
        firmwarePath,
        GENERIC_READ,
        FILE_SHARE_READ,
        OPEN_EXISTING,
        nullptr);
    if (firmwareFile == INVALID_HANDLE_VALUE)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    LARGE_INTEGER firmwareSize = {};
    if (!GetFileSizeEx(firmwareFile, &firmwareSize) ||
        firmwareSize.QuadPart <= 0 ||
        firmwareSize.QuadPart > NV_U32_MAX)
    {
        CloseHandle(firmwareFile);
        return NV_ERR_INVALID_DATA;
    }

    void* firmware = HeapAlloc(
        GetProcessHeap(),
        0,
        static_cast<SIZE_T>(firmwareSize.QuadPart));
    if (firmware == nullptr)
    {
        CloseHandle(firmwareFile);
        return NV_ERR_NO_MEMORY;
    }

    DWORD bytesRead = 0;
    NV_STATUS status = ReadFile(
        firmwareFile,
        firmware,
        static_cast<DWORD>(firmwareSize.QuadPart),
        &bytesRead,
        nullptr) && bytesRead == firmwareSize.QuadPart
        ? NvRmInitializeGpuFirmware(
            gpuContext,
            firmware,
            static_cast<NvU32>(firmwareSize.QuadPart))
        : NV_ERR_OPERATING_SYSTEM;

    HeapFree(GetProcessHeap(), 0, firmware);
    CloseHandle(firmwareFile);
    return status;
}
#endif

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
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType,
    NV_PLATFORM_RM_MEMORY_PURPOSE purpose,
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
    auto* deviceContext = static_cast<DeviceContext*>(context);
    if (InterlockedCompareExchange(&deviceContext->surpriseRemoved, 0, 0) != 0)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "DMA common buffer rejected: device surprise removed");
        return NV_ERR_GPU_IS_LOST;
    }
    if (purpose != NV_PLATFORM_RM_MEMORY_PURPOSE_DEFAULT &&
        purpose != NV_PLATFORM_RM_MEMORY_PURPOSE_CACHED_PIXEL_STAGING)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "DMA common buffer: invalid allocation purpose=%u", purpose);
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (purpose == NV_PLATFORM_RM_MEMORY_PURPOSE_CACHED_PIXEL_STAGING)
    {
        switch (cacheType)
        {
        case NV_PLATFORM_RM_MEMORY_CACHED:
            configuration.CacheType = MmCached;
            break;
        case NV_PLATFORM_RM_MEMORY_UNCACHED:
            configuration.CacheType = MmNonCached;
            break;
        case NV_PLATFORM_RM_MEMORY_WRITE_COMBINED:
            configuration.CacheType = MmWriteCombined;
            break;
        default:
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "DMA common buffer: invalid RM cache type=%u", cacheType);
            return NV_ERR_INVALID_ARGUMENT;
        }
    }
    WDFCOMMONBUFFER commonBuffer = nullptr;
    const NTSTATUS status = WdfCommonBufferCreateWithConfig(
        deviceContext->dmaEngine.Handle(),
        static_cast<SIZE_T>(size),
        &configuration,
        WDF_NO_OBJECT_ATTRIBUTES,
        &commonBuffer);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "DMA common buffer allocation: size=%llu cache=%u result=%!STATUS!",
            size, configuration.CacheType, status);
        return status == STATUS_INSUFFICIENT_RESOURCES
            ? NV_ERR_NO_MEMORY
            : NV_ERR_OPERATING_SYSTEM;
    }

    *providerAllocation = commonBuffer;
    *address = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer);
    *logicalAddress = static_cast<NvU64>(
        WdfCommonBufferGetAlignedLogicalAddress(commonBuffer).QuadPart);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "DMA common buffer allocated: purpose=%u size=%llu requestedCache=%u WDFCache=%u address=%p",
        purpose, size, cacheType, configuration.CacheType, *address);
    return NV_OK;
}

void FreeDmaBuffer(void*, void* providerAllocation)
{
    if (providerAllocation != nullptr)
    {
        WdfObjectDelete(static_cast<WDFCOMMONBUFFER>(providerAllocation));
    }
}

#if defined(NV_RM_USER_LINK_PROBE)
NvBool IsNvKmsDeviceRemoved(void* context)
{
    return IsHardwareRemoved(static_cast<WDFDEVICE>(context)) ? NV_TRUE : NV_FALSE;
}
#endif

NTSTATUS CreateRmGpuContext(_Inout_ DeviceContext* context) noexcept
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

    const ULONG barCount = context->bars.Count();
    for (ULONG barIndex = 0; barIndex < barCount; ++barIndex)
    {
        const auto& bar = context->bars[barIndex];
        const auto& range = bar.Resource();
        attachInfo.apertures[barIndex].physicalAddress =
            static_cast<NvU64>(range.physicalAddress.QuadPart);
        attachInfo.apertures[barIndex].length = range.length;
        attachInfo.apertures[barIndex].kernelAddress = bar.Address();
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_PCI,
            "RM attach BAR: index=%u translated=0x%llX length=0x%llX",
            barIndex, range.physicalAddress.QuadPart, range.length);
    }

    attachInfo.apertureCount = static_cast<NvU8>(barCount);
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
#if defined(NV_RM_USER_LINK_PROBE)
    if (context->rmGpuAttached &&
        InterlockedCompareExchange(&context->surpriseRemoved, 0, 0) != 0)
    {
        const NV_STATUS status = NvRmMarkGpuLost(context->rmGpuContext);
        TraceEvents(status == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER, "RM surprise-removal disconnect: 0x%08X", status);
    }
    if (context->nvKmsInitialized)
    {
        NvKmsWindowsSetDisplayChangeCallback(nullptr, nullptr);
        if (context->displayChangeWorkItem != nullptr)
        {
            WdfWorkItemFlush(context->displayChangeWorkItem);
        }
        NvKmsWindowsShutdown();
        context->nvKmsInitialized = false;
    }

    if (context->rmDeviceOpened)
    {
        const NV_STATUS status = NvRmCloseGpuDevice(context->rmClientHandle);
        TraceEvents(
            status == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "NvRmCloseGpuDevice: 0x%08X client=0x%08X",
            status,
            context->rmClientHandle);
        context->rmDeviceOpened = false;
        context->rmClientHandle = 0;
        context->rmDeviceHandle = 0;
        context->rmSubdeviceHandle = 0;
    }

    if (context->rmGpuStateLoaded)
    {
        const NV_STATUS status = NvRmUnloadGpuState(context->rmGpuContext);
        TraceEvents(
            status == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "NvRmUnloadGpuState: 0x%08X",
            status);
        context->rmGpuStateLoaded = false;
    }

    if (context->rmGpuAttached)
    {
        const NV_STATUS status = NvRmDetachGpuContext(
            context->rmGpuContext,
            context->rmGpuInstance,
            context->rmDeviceInstance,
            context->rmGpuId);
        NvKmsWindowsUnregisterGpu(context->rmGpuId);
        TraceEvents(
            status == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "NvRmDetachGpuContext: 0x%08X instance=%u device=%u gpuId=0x%08X",
            status,
            context->rmGpuInstance,
            context->rmDeviceInstance,
            context->rmGpuId);
        context->rmGpuAttached = false;
        context->rmGpuInstance = NV_U32_MAX;
        context->rmDeviceInstance = NV_U32_MAX;
        context->rmGpuId = 0;
    }
#endif

    NvPlatformRmGpuMarkShutdown(context->rmGpuContext);
    NvPlatformRmGpuContextDestroy(context->rmGpuContext);
    context->rmGpuContext = nullptr;
}

#if defined(NV_RM_USER_LINK_PROBE)
NV_STATUS AttachRmGpuContext(DeviceContext* context) noexcept
{
    context->rmGpuInstance = NV_U32_MAX;
    context->rmDeviceInstance = NV_U32_MAX;
    context->rmGpuId = 0;
    const NV_STATUS status = NvRmAttachGpuContext(
        context->rmGpuContext,
        &context->rmGpuInstance,
        &context->rmDeviceInstance,
        &context->rmGpuId);
    TraceEvents(status == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvRmAttachGpuContext: 0x%08X instance=%u device=%u gpuId=0x%08X",
        status, context->rmGpuInstance, context->rmDeviceInstance, context->rmGpuId);
    context->rmGpuAttached = status == NV_OK;
    return status;
}
#endif

} // namespace

_Use_decl_annotations_
NTSTATUS DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DeviceAdd");

    WdfDeviceInitSetPowerPolicyOwnership(deviceInit, TRUE);

    IDD_CX_CLIENT_CONFIG iddConfiguration;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfiguration);
    iddConfiguration.EvtIddCxAdapterInitFinished = AdapterInitFinished;
    iddConfiguration.EvtIddCxAdapterCommitModes = AdapterCommitModes;
    iddConfiguration.EvtIddCxParseMonitorDescription = ParseMonitorDescription;
    iddConfiguration.EvtIddCxMonitorGetDefaultDescriptionModes =
        GetDefaultDescriptionModes;
    iddConfiguration.EvtIddCxMonitorQueryTargetModes = QueryTargetModes;
    iddConfiguration.EvtIddCxMonitorAssignSwapChain = AssignSwapChain;
    iddConfiguration.EvtIddCxMonitorUnassignSwapChain = UnassignSwapChain;
    iddConfiguration.EvtIddCxMonitorSetGammaRamp = SetGammaRamp;
    iddConfiguration.EvtIddCxMonitorI2CTransmit = MonitorI2cTransmit;
    iddConfiguration.EvtIddCxMonitorI2CReceive = MonitorI2cReceive;
    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorI2CTransmitAndReceive))
    {
        iddConfiguration.EvtIddCxMonitorI2CTransmitAndReceive = MonitorI2cTransmitAndReceive;
    }

        if (IDD_IS_FIELD_AVAILABLE(
            IDD_CX_CLIENT_CONFIG,
            EvtIddCxParseMonitorDescription2))
        {
        iddConfiguration.EvtIddCxParseMonitorDescription2 =
            ParseMonitorDescription2;
        }
        if (IDD_IS_FIELD_AVAILABLE(
            IDD_CX_CLIENT_CONFIG,
            EvtIddCxAdapterQueryTargetInfo))
        {
        iddConfiguration.EvtIddCxAdapterQueryTargetInfo =
            AdapterQueryTargetInfo;
        }
        if (IDD_IS_FIELD_AVAILABLE(
            IDD_CX_CLIENT_CONFIG,
            EvtIddCxAdapterCommitModes2))
        {
        iddConfiguration.EvtIddCxAdapterCommitModes2 = AdapterCommitModes2;
        }
        if (IDD_IS_FIELD_AVAILABLE(
            IDD_CX_CLIENT_CONFIG,
            EvtIddCxMonitorQueryTargetModes2))
        {
        iddConfiguration.EvtIddCxMonitorQueryTargetModes2 = QueryTargetModes2;
        }
        if (IDD_IS_FIELD_AVAILABLE(
            IDD_CX_CLIENT_CONFIG,
            EvtIddCxMonitorSetDefaultHdrMetaData))
        {
        iddConfiguration.EvtIddCxMonitorSetDefaultHdrMetaData =
            SetDefaultHdrMetadata;
        }

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfiguration);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "IddCxDeviceInitConfig failed: %!STATUS!",
            status);
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = PrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = EnterD0;
    pnpCallbacks.EvtDeviceD0Exit = ExitD0;
    pnpCallbacks.EvtDeviceSurpriseRemoval = SurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = DeviceCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "WdfDeviceCreate failed: %!STATUS!",
            status);
        return status;
    }

    void* contextStorage = GetDeviceContext(device);
    auto* context = new (contextStorage) DeviceContext{};
    InitializeSRWLock(&context->lifecycleLock);

    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, DisplayChangeWorkItem);
    WDF_OBJECT_ATTRIBUTES workItemAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&workItemAttributes);
    workItemAttributes.ParentObject = device;
    status = WdfWorkItemCreate(
        &workItemConfig,
        &workItemAttributes,
        &context->displayChangeWorkItem);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(device);
        return status;
    }

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
    context->rmInitialized = true;
#endif

    status = IddCxDeviceInitialize(device);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxDeviceInitialize: %!STATUS!",
        status);
    // This query must precede every IddCxAdapterInitAsync call.
    if (NT_SUCCESS(status) &&
        IDD_IS_FUNCTION_AVAILABLE(IddCxCheckOsFeatureSupport))
    {
        static INIT_ONCE queryOnce = INIT_ONCE_STATIC_INIT;
        static bool supported = false;
        const BOOL queried = InitOnceExecuteOnce(&queryOnce,
            [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
                IDARG_OUT_FEATURES_SUPPORTED features = {};
                features.Size = sizeof(features);
                const NTSTATUS featureStatus = IddCxCheckOsFeatureSupport(&features);
                supported = NT_SUCCESS(featureStatus) &&
                    (features.Features_1_11 & IDDCX_DEVICE_FEATURES_1_11_D3D12) != 0;
                TraceEvents(NT_SUCCESS(featureStatus) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
                    TRACE_DRIVER, "IddCx OS features: result=%!STATUS! d3d12=%u flags=0x%08X",
                    featureStatus, supported, features.Features_1_11);
                return TRUE;
            }, nullptr, nullptr);
        if (!queried)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "IddCx OS feature initialization failed: error=%u", GetLastError());
        }
        context->d3d12SwapChainSupported = queried && supported;
    }
    return status;
}

_Use_decl_annotations_
void DeviceCleanup(WDFOBJECT deviceObject)
{
    DeviceContext* context = GetDeviceContext(deviceObject);
    context->adapter.Reset();
    context->deviceReset.Reset();
#if defined(NV_RM_USER_LINK_PROBE)
    if (context->rmInitialized)
    {
        RmDestroyRm();
        context->rmInitialized = false;
    }
#endif
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
    if (IsHardwareRemoved(device))
    {
        return STATUS_DEVICE_REMOVED;
    }
    const ULONG resourceCount = WdfCmResourceListGetCount(resourcesTranslated);
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "PrepareHardware: resources=%u",
        resourceCount);

    if (resourceCount == 0)
    {
        return STATUS_SUCCESS;
    }

    auto cleanup = wil::scope_exit([&]() noexcept {
        ReleaseHardware(device, resourcesTranslated);
    });
    NTSTATUS status = context->configSpace.Initialize(device);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "ConfigSpace::Initialize: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    const PciIdentity* identity = context->configSpace.Identity();
    if (identity == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI, "PCI identity unavailable after initialization");
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
        return status;
    }

    status = context->configSpace.EnableMemorySpace();
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "ConfigSpace::EnableMemorySpace: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = context->bars.Initialize(
        device,
        resourcesRaw,
        resourcesTranslated,
        context->configSpace);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "PciBars::Initialize: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = context->dmaEngine.Initialize(device);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "DmaEngine::Initialize: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    const NV_PLATFORM_DMA_PROVIDER dmaProvider{
        context,
        AllocateDmaBuffer,
        FreeDmaBuffer,
    };
    const NV_STATUS dmaStatus = NvPlatformRegisterDmaProvider(
        &dmaProvider, &context->dmaRegistration);
    TraceEvents(
        dmaStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvPlatformRegisterDmaProvider: 0x%08X",
        dmaStatus);
    if (dmaStatus != NV_OK)
    {
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

#if defined(NV_RM_USER_LINK_PROBE)
    void* dmaAllocation = nullptr;
    void* dmaAddress = nullptr;
    NvU64 dmaLogicalAddress = 0;
    const NV_STATUS dmaProbeStatus = AllocateDmaBuffer(
        context,
        4096,
        4096,
        NV_PLATFORM_RM_MEMORY_UNCACHED,
        NV_PLATFORM_RM_MEMORY_PURPOSE_DEFAULT,
        &dmaAllocation,
        &dmaAddress,
        &dmaLogicalAddress);
    TraceEvents(
        dmaProbeStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "DMA common-buffer probe: status=0x%08X cpu=%p logical=0x%llX",
        dmaProbeStatus,
        dmaAddress,
        dmaLogicalAddress);
    FreeDmaBuffer(context, dmaAllocation);
    if (dmaProbeStatus != NV_OK)
    {
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
#endif

    status = context->interrupt.Initialize(
        device,
        resourcesRaw,
        resourcesTranslated);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "Interrupt::Initialize: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = CreateRmGpuContext(context);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "CreateRmGpuContext: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

#if defined(NV_RM_USER_LINK_PROBE)
    constexpr SIZE_T PmcBoot1Offset = 0x00000004;
    const auto& bar0 = context->bars[PciBars::Bar0Index];
    const ULONG directBoot1 =
        bar0.Aperture().ReadRegister32(PmcBoot1Offset);
    const ULONG routedBoot1 = NvPlatformRmReadRegister32(
        static_cast<const volatile UCHAR*>(
            bar0.Address()) + PmcBoot1Offset);
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "RM pre-attach BOOT_1: direct=0x%08X routed=0x%08X",
        directBoot1,
        routedBoot1);

    const NV_STATUS attachStatus = AttachRmGpuContext(context);
    if (attachStatus != NV_OK)
    {
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    context->deviceReset.Initialize(device);

    NV_STATUS firmwareStatus = InitializeRmGpuFirmware(
        context->rmGpuContext);
    TraceEvents(
        firmwareStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvRmInitializeGpuFirmware: 0x%08X",
        firmwareStatus);
    if (firmwareStatus == NV_ERR_TIMEOUT && !IsHardwareRemoved(device))
    {
        // Retain framework resources; rebuild only failed RM state and its BAR1 leases.
        DestroyRmGpuContext(context);
        context->bars.SuspendDemandMappings();
        const NTSTATUS resetStatus = context->deviceReset.ResetDeviceInPlace();
        if (resetStatus == STATUS_NOT_SUPPORTED)
        {
            context->deviceReset.ResetDevice(0);
            return STATUS_DRIVER_INTERNAL_ERROR;
        }
        if (!NT_SUCCESS(resetStatus) || resetStatus == STATUS_PENDING)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Firmware startup recovery reset did not complete: %!STATUS!", resetStatus);
            return STATUS_DRIVER_INTERNAL_ERROR;
        }
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "Firmware startup recovery: in-place reset completed; rebuilding RM and retrying once");
        status = context->configSpace.EnableMemorySpace();
        if (NT_SUCCESS(status))
        {
            status = context->bars.RestoreDemandMappings();
        }
        if (NT_SUCCESS(status))
        {
            status = CreateRmGpuContext(context);
        }
        TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER, "Firmware startup recovery RM context: %!STATUS!", status);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (AttachRmGpuContext(context) != NV_OK)
        {
            return STATUS_DRIVER_INTERNAL_ERROR;
        }
        firmwareStatus = InitializeRmGpuFirmware(context->rmGpuContext);
        TraceEvents(firmwareStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER, "Firmware startup recovery retry: 0x%08X", firmwareStatus);
    }
    if (firmwareStatus != NV_OK)
    {
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    context->rmGpuStateLoaded = true;

    const NV_STATUS modesetRegisterStatus = NvKmsWindowsRegisterGpu(
            context->rmGpuId,
            identity->domain,
            identity->bus,
            identity->device,
            identity->function,
            context->rmGpuContext,
            IsNvKmsDeviceRemoved,
            device);
    if (modesetRegisterStatus != NV_OK)
    {
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    const NTSTATUS workerStatus = StartNvKmsInitialization(device);
    if (!NT_SUCCESS(workerStatus))
    {
        return workerStatus;
    }
#endif

    cleanup.release();
    return STATUS_SUCCESS;
}

DWORD WINAPI InitializeNvKms(void* parameter)
{
#if defined(NV_RM_USER_LINK_PROBE)
    const WDFDEVICE device = static_cast<WDFDEVICE>(parameter);
    DeviceContext* const context = GetDeviceContext(device);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "NVKMS worker begin");

    if (InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) != 0)
    {
        return 0;
    }

    const NV_STATUS modesetProbeStatus = NvKmsWindowsInitialize();
    TraceEvents(
        modesetProbeStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvKmsWindowsInitialize: 0x%08X",
        modesetProbeStatus);
    if (modesetProbeStatus != NV_OK)
    {
        return 0;
    }
    context->nvKmsInitialized = true;
    if (InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) != 0)
    {
        return 0;
    }
    NvKmsWindowsSetDisplayChangeCallback(
        [](void* callbackContext)
        {
            const WDFDEVICE callbackDevice =
                static_cast<WDFDEVICE>(callbackContext);
            DeviceContext* const callbackDeviceContext =
                GetDeviceContext(callbackDevice);
            if (callbackDeviceContext->displayChangeWorkItem != nullptr &&
                !IsHardwareRemoved(callbackDevice))
            {
                WdfWorkItemEnqueue(
                    callbackDeviceContext->displayChangeWorkItem);
            }
        },
        device);

    NV_KMS_WINDOWS_DISPLAY_INFO displayInfo = {};
    const NV_STATUS displayProbeStatus = NvKmsWindowsProbeDisplays(&displayInfo);
    TraceEvents(
        displayProbeStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvKmsWindowsProbeDisplays: 0x%08X connectors=%u displays=%u connected=%u EDID=%u checksum=0x%02X",
        displayProbeStatus,
        displayInfo.connectorCount,
        displayInfo.displayCount,
        displayInfo.connectedCount,
        displayInfo.edidBytes,
        displayInfo.edidChecksum);
    if (displayProbeStatus != NV_OK ||
        InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) != 0)
    {
        return 0;
    }

    NV_KMS_WINDOWS_GPU_INFO modesetGpuInfo = {};
    const NV_STATUS modesetEnumerationStatus =
        NvKmsWindowsProbeGpuEnumeration(&modesetGpuInfo);
    TraceEvents(
        modesetEnumerationStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvKmsWindowsProbeGpuEnumeration: 0x%08X count=%u gpuId=0x%08X pci=%04X:%02X:%02X.%u",
        modesetEnumerationStatus,
        modesetGpuInfo.count,
        modesetGpuInfo.gpuId,
        modesetGpuInfo.domain,
        modesetGpuInfo.bus,
        modesetGpuInfo.slot,
        modesetGpuInfo.function);
    if (modesetEnumerationStatus != NV_OK)
    {
        return 0;
    }

    NV_RM_GPU_OPEN_STAGE openStage = NV_RM_GPU_OPEN_STAGE_ROOT_CLIENT;
    const NV_STATUS openStatus = NvRmOpenGpuDevice(
        context->rmGpuContext,
        context->rmGpuId,
        context->rmDeviceInstance,
        &context->rmClientHandle,
        &context->rmDeviceHandle,
        &context->rmSubdeviceHandle,
        &openStage);
    TraceEvents(
        openStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvRmOpenGpuDevice: 0x%08X stage=%u client=0x%08X device=0x%08X subdevice=0x%08X",
        openStatus,
        static_cast<unsigned>(openStage),
        context->rmClientHandle,
        context->rmDeviceHandle,
        context->rmSubdeviceHandle);
    context->rmDeviceOpened = openStatus == NV_OK;
    if (openStatus != NV_OK ||
        InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) != 0)
    {
        return 0;
    }
    InterlockedExchange(&context->nvKmsWorkCompleted, 1);

    AcquireSRWLockExclusive(&context->lifecycleLock);
    NTSTATUS adapterStatus = STATUS_SUCCESS;
    if (InterlockedCompareExchange(&context->d0Active, 0, 0) != 0 &&
        InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) == 0)
    {
        adapterStatus = context->adapter.NotifyDisplayReady();
    }
    ReleaseSRWLockExclusive(&context->lifecycleLock);
    TraceEvents(
        NT_SUCCESS(adapterStatus) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NVKMS worker complete: %!STATUS!",
        adapterStatus);
    return 0;
#else
    UNREFERENCED_PARAMETER(parameter);
    return 0;
#endif
}

NTSTATUS StartNvKmsInitialization(_In_ WDFDEVICE device) noexcept
{
#if defined(NV_RM_USER_LINK_PROBE)
    DeviceContext* const context = GetDeviceContext(device);
    if (IsHardwareRemoved(device))
    {
        return STATUS_DEVICE_REMOVED;
    }
    if (context->nvKmsThread != nullptr)
    {
        return STATUS_SUCCESS;
    }

    InterlockedExchange(&context->nvKmsWorkCanceled, 0);
    if (IsHardwareRemoved(device))
    {
        InterlockedExchange(&context->nvKmsWorkCanceled, 1);
        return STATUS_DEVICE_REMOVED;
    }
    InterlockedExchange(&context->nvKmsWorkCompleted, 0);
    context->nvKmsThread = CreateThread(
        nullptr,
        0,
        InitializeNvKms,
        device,
        0,
        nullptr);
    return context->nvKmsThread != nullptr
        ? STATUS_SUCCESS
        : STATUS_INSUFFICIENT_RESOURCES;
#else
    UNREFERENCED_PARAMETER(device);
    return STATUS_SUCCESS;
#endif
}

_Use_decl_annotations_
void DisplayChangeWorkItem(WDFWORKITEM workItem)
{
    const WDFDEVICE device = static_cast<WDFDEVICE>(
        WdfWorkItemGetParentObject(workItem));
    DeviceContext* const context = GetDeviceContext(device);

    AcquireSRWLockExclusive(&context->lifecycleLock);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (context->nvKmsInitialized &&
        InterlockedCompareExchange(&context->d0Active, 0, 0) != 0 &&
        InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) == 0)
    {
        status = context->adapter.RefreshDisplays();
    }
    ReleaseSRWLockExclusive(&context->lifecycleLock);

    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "Display change refresh: %!STATUS!",
        status);
}

_Use_decl_annotations_
NTSTATUS ReleaseHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesTranslated);

    DeviceContext* context = GetDeviceContext(device);
    InterlockedExchange(&context->nvKmsWorkCanceled, 1);
    InterlockedExchange(&context->d0Active, 0);
    if (context->nvKmsThread != nullptr)
    {
        WaitForSingleObject(context->nvKmsThread, INFINITE);
        CloseHandle(context->nvKmsThread);
        context->nvKmsThread = nullptr;
    }
    InterlockedExchange(&context->nvKmsWorkCompleted, 0);
#if defined(NV_RM_USER_LINK_PROBE)
    if (context->rmGpuAttached && IsHardwareRemoved(device))
    {
        const NV_STATUS status = NvRmMarkGpuLost(context->rmGpuContext);
        TraceEvents(status == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER, "RM disconnect before output cleanup: 0x%08X", status);
    }
#endif
    context->adapter.Reset();
    DestroyRmGpuContext(context);
    context->deviceReset.Reset();
    context->interrupt.Reset();
    NvPlatformUnregisterDmaProvider(context->dmaRegistration);
    context->dmaRegistration = nullptr;
    context->dmaEngine.Reset();
    context->bars.Reset();
    NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
    context->pciConfigRegistration = nullptr;
    context->configSpace.Reset();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EnterD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    DeviceContext* const context = GetDeviceContext(device);
    AcquireSRWLockExclusive(&context->lifecycleLock);
    if (IsHardwareRemoved(device))
    {
        ReleaseSRWLockExclusive(&context->lifecycleLock);
        return STATUS_DEVICE_REMOVED;
    }
    InterlockedExchange(&context->d0Active, 1);
    const NTSTATUS status = context->adapter.EnterD0(device);
    ReleaseSRWLockExclusive(&context->lifecycleLock);

    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "EnterD0: %!STATUS! previous=%u",
        status,
        previousState);
    return status;
}

_Use_decl_annotations_
NTSTATUS ExitD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE targetState)
{
    UNREFERENCED_PARAMETER(targetState);
    DeviceContext* const context = GetDeviceContext(device);
    AcquireSRWLockExclusive(&context->lifecycleLock);
    InterlockedExchange(&context->d0Active, 0);
    context->adapter.ExitD0();
    ReleaseSRWLockExclusive(&context->lifecycleLock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void SurpriseRemoval(WDFDEVICE device)
{
    DeviceContext* const context = GetDeviceContext(device);
    // WDF can invoke this concurrently with prepare/release; do not touch their owners.
    InterlockedExchange(&context->surpriseRemoved, 1);
    InterlockedExchange(&context->nvKmsWorkCanceled, 1);
    InterlockedExchange(&context->d0Active, 0);
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
        "Device surprise removed: hardware access and initialization canceled");
}

} // namespace nv::iddcx