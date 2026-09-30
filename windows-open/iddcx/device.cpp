#include "device.hpp"
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

UCHAR ReadMmio8(void* context, SIZE_T offset)
{
    return static_cast<RegisterAperture*>(context)->ReadRegister8(offset);
}

USHORT ReadMmio16(void* context, SIZE_T offset)
{
    return static_cast<RegisterAperture*>(context)->ReadRegister16(offset);
}

ULONG ReadMmio32(void* context, SIZE_T offset)
{
    return static_cast<RegisterAperture*>(context)->ReadRegister32(offset);
}

void ReadMmioBuffer32(
    void* context,
    SIZE_T offset,
    ULONG* buffer,
    ULONG count)
{
    static_cast<RegisterAperture*>(context)->ReadRegisterBuffer32(
        offset, buffer, count);
}

void WriteMmio8(void* context, SIZE_T offset, UCHAR value)
{
    static_cast<RegisterAperture*>(context)->WriteRegister8(offset, value);
}

void WriteMmio16(void* context, SIZE_T offset, USHORT value)
{
    static_cast<RegisterAperture*>(context)->WriteRegister16(offset, value);
}

void WriteMmio32(void* context, SIZE_T offset, ULONG value)
{
    static_cast<RegisterAperture*>(context)->WriteRegister32(offset, value);
}

void ResetMmioApertures(_Inout_ DeviceContext* context) noexcept
{
    NvPlatformUnregisterMmioAperture(context->bar2MmioRegistration);
    context->bar2MmioRegistration = nullptr;
    context->bar2Aperture.Reset();
    NvPlatformUnregisterMmioAperture(context->mmioRegistration);
    context->mmioRegistration = nullptr;
    context->registerAperture.Reset();
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
    ULONG nextMemoryResource = 0;
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
                nextMemoryResource = resourceIndex + 1;
                break;
            }
        }

        if (barRange == nullptr)
        {
            for (ULONG resourceIndex = nextMemoryResource;
                 resourceIndex < resourceCount;
                 ++resourceIndex)
            {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR candidate =
                    WdfCmResourceListGetDescriptor(
                        resourcesTranslated,
                        resourceIndex);
                if (candidate != nullptr &&
                    candidate->Type == CmResourceTypeMemory)
                {
                    barRange = candidate;
                    nextMemoryResource = resourceIndex + 1;
                    TraceEvents(
                        TRACE_LEVEL_INFORMATION,
                        TRACE_PCI,
                        "RM attach BAR translated fallback: index=%u config=0x%llX assigned=0x%llX",
                        barIndex,
                        barAddress.QuadPart,
                        barRange->u.Memory.Start.QuadPart);
                    break;
                }
            }
        }

        if (barRange == nullptr)
        {
            TraceEvents(
                TRACE_LEVEL_WARNING,
                TRACE_PCI,
                "RM attach BAR unassigned: index=%u config=0x%llX",
                barIndex,
                barAddress.QuadPart);
            continue;
        }

        attachInfo.apertures[barIndex].physicalAddress =
            static_cast<NvU64>(barRange->u.Memory.Start.QuadPart);
        attachInfo.apertures[barIndex].length = barRange->u.Memory.Length;
    }

    attachInfo.apertureCount = static_cast<NvU8>(barCount);
    attachInfo.apertures[0].kernelAddress = context->registerAperture.Address();
    if (barCount > 2)
    {
        attachInfo.apertures[2].kernelAddress = context->bar2Aperture.Address();
    }
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
    if (context->nvKmsInitialized)
    {
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

    if (context->rmGpuAttached)
    {
        NvKmsWindowsUnregisterGpu(context->rmGpuId);
        const NV_STATUS status = NvRmDetachGpuContext(
            context->rmGpuContext,
            context->rmGpuInstance,
            context->rmDeviceInstance,
            context->rmGpuId);
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

} // namespace

_Use_decl_annotations_
NTSTATUS DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DeviceAdd");

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
    return status;
}

_Use_decl_annotations_
void DeviceCleanup(WDFOBJECT deviceObject)
{
    DeviceContext* context = GetDeviceContext(deviceObject);
    context->adapter.Reset();
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

    status = context->configSpace.EnableMemorySpace();
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "ConfigSpace::EnableMemorySpace: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    status = context->registerAperture.Initialize(
        device,
        resourcesTranslated,
        context->configSpace);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "RegisterAperture::Initialize: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    const NV_PLATFORM_MMIO_ACCESS mmioAccess = {
        &context->registerAperture,
        ReadMmio8,
        ReadMmio16,
        ReadMmio32,
        ReadMmioBuffer32,
        WriteMmio8,
        WriteMmio16,
        WriteMmio32,
    };
    status = NvPlatformRegisterMmioAperture(
        static_cast<ULONGLONG>(
            context->configSpace.RegisterBarAddress().QuadPart),
        context->registerAperture.Address(),
        context->registerAperture.Length(),
        NV_PLATFORM_MMIO_CACHE_UNCACHED,
        &mmioAccess,
        &context->mmioRegistration);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "NvPlatformRegisterMmioAperture: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        context->registerAperture.Reset();
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    status = context->bar2Aperture.InitializeMemoryBar(
        device,
        resourcesTranslated,
        context->configSpace,
        2,
        MmWriteCombined);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "BAR2 aperture initialization: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    const NV_PLATFORM_MMIO_ACCESS bar2Access = {
        &context->bar2Aperture,
        ReadMmio8,
        ReadMmio16,
        ReadMmio32,
        ReadMmioBuffer32,
        WriteMmio8,
        WriteMmio16,
        WriteMmio32,
    };
    status = NvPlatformRegisterMmioAperture(
        static_cast<ULONGLONG>(
            context->configSpace.MemoryBarAddress(2).QuadPart),
        context->bar2Aperture.Address(),
        context->bar2Aperture.Length(),
        NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &bar2Access,
        &context->bar2MmioRegistration);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "BAR2 MMIO registration: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
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
        ResetMmioApertures(context);
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
    TraceEvents(
        dmaStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvPlatformRegisterDmaProvider: 0x%08X",
        dmaStatus);
    if (dmaStatus != NV_OK)
    {
        context->dmaEngine.Reset();
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

#if defined(NV_RM_USER_LINK_PROBE)
    void* dmaAllocation = nullptr;
    void* dmaAddress = nullptr;
    NvU64 dmaLogicalAddress = 0;
    const NV_STATUS dmaProbeStatus = AllocateDmaBuffer(
        &context->dmaEngine,
        4096,
        4096,
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
    FreeDmaBuffer(&context->dmaEngine, dmaAllocation);
    if (dmaProbeStatus != NV_OK)
    {
        NvPlatformUnregisterDmaProvider(context->dmaRegistration);
        context->dmaRegistration = nullptr;
        context->dmaEngine.Reset();
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
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
        NvPlatformUnregisterDmaProvider(context->dmaRegistration);
        context->dmaRegistration = nullptr;
        context->dmaEngine.Reset();
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

    status = CreateRmGpuContext(context, resourcesTranslated);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "CreateRmGpuContext: %!STATUS!",
        status);
    if (!NT_SUCCESS(status))
    {
        context->interrupt.Reset();
        NvPlatformUnregisterDmaProvider(context->dmaRegistration);
        context->dmaRegistration = nullptr;
        context->dmaEngine.Reset();
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return status;
    }

#if defined(NV_RM_USER_LINK_PROBE)
    constexpr SIZE_T PmcBoot1Offset = 0x00000004;
    const ULONG directBoot1 =
        context->registerAperture.ReadRegister32(PmcBoot1Offset);
    const ULONG routedBoot1 = NvPlatformRmReadRegister32(
        static_cast<const volatile UCHAR*>(
            context->registerAperture.Address()) + PmcBoot1Offset);
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "RM pre-attach BOOT_1: direct=0x%08X routed=0x%08X",
        directBoot1,
        routedBoot1);

    context->rmGpuInstance = NV_U32_MAX;
    context->rmDeviceInstance = NV_U32_MAX;
    context->rmGpuId = 0;
    const NV_STATUS attachStatus = NvRmAttachGpuContext(
        context->rmGpuContext,
        &context->rmGpuInstance,
        &context->rmDeviceInstance,
        &context->rmGpuId);
    TraceEvents(
        attachStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvRmAttachGpuContext: 0x%08X instance=%u device=%u gpuId=0x%08X",
        attachStatus,
        context->rmGpuInstance,
        context->rmDeviceInstance,
        context->rmGpuId);
    if (attachStatus != NV_OK)
    {
        DestroyRmGpuContext(context);
        context->interrupt.Reset();
        NvPlatformUnregisterDmaProvider(context->dmaRegistration);
        context->dmaRegistration = nullptr;
        context->dmaEngine.Reset();
        ResetMmioApertures(context);
        NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
        context->pciConfigRegistration = nullptr;
        context->configSpace.Reset();
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    context->rmGpuAttached = true;

    const NV_STATUS firmwareStatus = InitializeRmGpuFirmware(
        context->rmGpuContext);
    TraceEvents(
        firmwareStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "NvRmInitializeGpuFirmware: 0x%08X",
        firmwareStatus);
    if (firmwareStatus != NV_OK)
    {
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
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    const NV_STATUS modesetRegisterStatus = identity != nullptr
        ? NvKmsWindowsRegisterGpu(
            context->rmGpuId,
            identity->domain,
            identity->bus,
            identity->device,
            identity->function,
            context->rmGpuContext)
        : NV_ERR_INVALID_STATE;
    if (modesetRegisterStatus != NV_OK)
    {
        DestroyRmGpuContext(context);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    InterlockedExchange(&context->nvKmsWorkCanceled, 0);
    InterlockedExchange(&context->nvKmsWorkCompleted, 0);
    context->nvKmsThread = CreateThread(
        nullptr,
        0,
        InitializeNvKms,
        device,
        0,
        nullptr);
    if (context->nvKmsThread == nullptr)
    {
        DestroyRmGpuContext(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
#endif

    return STATUS_SUCCESS;
}

DWORD WINAPI InitializeNvKms(void* parameter)
{
#if defined(NV_RM_USER_LINK_PROBE)
    const WDFDEVICE device = static_cast<WDFDEVICE>(parameter);
    DeviceContext* const context = GetDeviceContext(device);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "NVKMS worker begin");

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
    if (openStatus != NV_OK ||
        InterlockedCompareExchange(&context->nvKmsWorkCanceled, 0, 0) != 0)
    {
        return 0;
    }
    context->rmDeviceOpened = true;
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

_Use_decl_annotations_
NTSTATUS ReleaseHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesTranslated);

    DeviceContext* context = GetDeviceContext(device);
    InterlockedExchange(&context->nvKmsWorkCanceled, 1);
    if (context->nvKmsThread != nullptr)
    {
        WaitForSingleObject(context->nvKmsThread, INFINITE);
        CloseHandle(context->nvKmsThread);
        context->nvKmsThread = nullptr;
    }
    InterlockedExchange(&context->nvKmsWorkCompleted, 0);
    context->adapter.Reset();
    DestroyRmGpuContext(context);
    context->interrupt.Reset();
    NvPlatformUnregisterDmaProvider(context->dmaRegistration);
    context->dmaRegistration = nullptr;
    context->dmaEngine.Reset();
    ResetMmioApertures(context);
    NvPlatformUnregisterPciConfig(context->pciConfigRegistration);
    context->pciConfigRegistration = nullptr;
    context->configSpace.Reset();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EnterD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    UNREFERENCED_PARAMETER(previousState);
    DeviceContext* const context = GetDeviceContext(device);
    AcquireSRWLockExclusive(&context->lifecycleLock);
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

} // namespace nv::iddcx