#include "adapter.hpp"
#include "nvkms_windows.h"
#include "trace.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <cstring>
#include <wrl/client.h>

#include "adapter.tmh"

namespace {

constexpr wchar_t EndpointFriendlyName[] = L"NVIDIA Indirect Display Adapter";
constexpr wchar_t EndpointManufacturerName[] = L"NVIDIA";
constexpr wchar_t EndpointModelName[] = L"Open GPU Kernel Modules";

void FillSignalInfo(
    _Out_ DISPLAYCONFIG_VIDEO_SIGNAL_INFO* signalInfo,
    _In_ const NV_KMS_WINDOWS_MODE& mode,
    _In_ bool monitorMode) noexcept
{
    signalInfo->pixelRate = mode.pixelClockHz;
    signalInfo->hSyncFreq.Numerator = mode.pixelClockHz;
    signalInfo->hSyncFreq.Denominator = mode.hTotal;
    signalInfo->vSyncFreq.Numerator = mode.refreshRateMillihertz;
    signalInfo->vSyncFreq.Denominator = 1000;
    signalInfo->activeSize.cx = mode.hVisible;
    signalInfo->activeSize.cy = mode.vVisible;
    signalInfo->totalSize.cx = mode.hTotal;
    signalInfo->totalSize.cy = mode.vTotal;
    signalInfo->videoStandard = 255;
    signalInfo->scanLineOrdering = mode.interlaced
        ? DISPLAYCONFIG_SCANLINE_ORDERING_INTERLACED_UPPERFIELDFIRST
        : DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    signalInfo->AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
}

IDDCX_MONITOR_MODE CreateMonitorMode(
    _In_ const NV_KMS_WINDOWS_MODE& displayMode,
    _In_ IDDCX_MONITOR_MODE_ORIGIN origin) noexcept
{
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(&mode.MonitorVideoSignalInfo, displayMode, true);
    return mode;
}

IDDCX_MONITOR_MODE2 CreateMonitorMode2(
    _In_ const NV_KMS_WINDOWS_MODE& displayMode,
    _In_ IDDCX_MONITOR_MODE_ORIGIN origin) noexcept
{
    IDDCX_MONITOR_MODE2 mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(&mode.MonitorVideoSignalInfo, displayMode, true);
    mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    return mode;
}

IDDCX_TARGET_MODE CreateTargetMode(
    _In_ const NV_KMS_WINDOWS_MODE& displayMode) noexcept
{
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(
        &mode.TargetVideoSignalInfo.targetVideoSignalInfo,
        displayMode,
        false);
    return mode;
}

IDDCX_TARGET_MODE2 CreateTargetMode2(
    _In_ const NV_KMS_WINDOWS_MODE& displayMode) noexcept
{
    IDDCX_TARGET_MODE2 mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(
        &mode.TargetVideoSignalInfo.targetVideoSignalInfo,
        displayMode,
        false);
    mode.RequiredBandwidth = mode.TargetVideoSignalInfo
        .targetVideoSignalInfo.pixelRate;
    mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    return mode;
}

DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY GetOutputTechnology(
    _In_ NvU32 connectorType) noexcept
{
    switch (connectorType)
    {
    case 0:
    case 7:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
    case 1:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15;
    case 2:
    case 3:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI;
    case 5:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
    case 6:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    default:
        return DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    }
}

NTSTATUS GetDisplayState(
    _In_ NvU32 connectedDisplayIndex,
    _Out_ NV_KMS_WINDOWS_DISPLAY_STATE* displayState) noexcept
{
    const NV_STATUS status =
        NvKmsWindowsGetDisplay(connectedDisplayIndex, displayState);
    if (status != NV_OK)
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "NvKmsWindowsGetDisplay[%u] failed: 0x%08X",
            connectedDisplayIndex,
            status);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS GetDisplayState(
    _In_ const IDDCX_MONITOR_DESCRIPTION& description,
    _Out_ NV_KMS_WINDOWS_DISPLAY_STATE* displayState) noexcept
{
    NvU32 displayCount = 0;
    if (description.pData == nullptr ||
        NvKmsWindowsGetConnectedDisplayCount(&displayCount) != NV_OK)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    for (NvU32 index = 0; index < displayCount; ++index)
    {
        NV_KMS_WINDOWS_DISPLAY_STATE candidate = {};
        if (NvKmsWindowsGetDisplay(index, &candidate) == NV_OK &&
            candidate.edidBytes == description.DataSize &&
            std::memcmp(candidate.edid, description.pData, candidate.edidBytes) == 0)
        {
            *displayState = candidate;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

NTSTATUS FillMonitorModeBuffer(
    _In_ const NV_KMS_WINDOWS_DISPLAY_STATE& displayState,
    _In_ UINT inputCount,
    _Out_writes_opt_(inputCount) IDDCX_MONITOR_MODE* modes,
    _In_ IDDCX_MONITOR_MODE_ORIGIN origin,
    _Out_ UINT* outputCount,
    _Out_ UINT* preferredModeIndex) noexcept
{
    *outputCount = displayState.modeCount;
    *preferredModeIndex = displayState.preferredModeIndex < displayState.modeCount
        ? displayState.preferredModeIndex
        : 0;
    if (inputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (modes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (inputCount < displayState.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (UINT index = 0; index < displayState.modeCount; ++index)
    {
        modes[index] = CreateMonitorMode(displayState.modes[index], origin);
    }
    return STATUS_SUCCESS;
}

NTSTATUS FillMonitorMode2Buffer(
    _In_ const NV_KMS_WINDOWS_DISPLAY_STATE& displayState,
    _In_ UINT inputCount,
    _Out_writes_opt_(inputCount) IDDCX_MONITOR_MODE2* modes,
    _Out_ UINT* outputCount,
    _Out_ UINT* preferredModeIndex) noexcept
{
    *outputCount = displayState.modeCount;
    *preferredModeIndex = displayState.preferredModeIndex < displayState.modeCount
        ? displayState.preferredModeIndex
        : 0;
    if (inputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (modes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (inputCount < displayState.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (UINT index = 0; index < displayState.modeCount; ++index)
    {
        modes[index] = CreateMonitorMode2(
            displayState.modes[index],
            IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    return STATUS_SUCCESS;
}

const NV_KMS_WINDOWS_MODE* FindDisplayMode(
    _In_ const NV_KMS_WINDOWS_DISPLAY_STATE& displayState,
    _In_ const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signalInfo) noexcept
{
    for (NvU32 index = 0; index < displayState.modeCount; ++index)
    {
        const NV_KMS_WINDOWS_MODE& mode = displayState.modes[index];
        if (mode.pixelClockHz == signalInfo.pixelRate &&
            mode.hVisible == signalInfo.activeSize.cx &&
            mode.vVisible == signalInfo.activeSize.cy &&
            mode.hTotal == signalInfo.totalSize.cx &&
            mode.vTotal == signalInfo.totalSize.cy)
        {
            return &mode;
        }
    }
    return nullptr;
}

const NV_KMS_WINDOWS_MODE* FindDisplayMode(
    _In_ const NV_KMS_WINDOWS_DISPLAY_STATE& displayState,
    _In_ NvU32 modeIndex) noexcept
{
    for (NvU32 index = 0; index < displayState.modeCount; ++index)
    {
        if (displayState.modes[index].kmsModeIndex == modeIndex)
        {
            return &displayState.modes[index];
        }
    }
    return nullptr;
}

void FillGradient(
    _Out_writes_bytes_(static_cast<size_t>(pitch) * height) void* pixels,
    _In_ NvU32 pitch,
    _In_ NvU32 width,
    _In_ NvU32 height,
    _In_ NvU32 outputIndex) noexcept
{
    std::memset(pixels, 0, static_cast<size_t>(pitch) * height);
    auto* const bytes = static_cast<NvU8*>(pixels);
    for (NvU32 y = 0; y < height; ++y)
    {
        auto* const row = reinterpret_cast<NvU32*>(bytes + (pitch * y));
        const NvU32 green = height > 1 ? (255U * y) / (height - 1) : 0;
        for (NvU32 x = 0; x < width; ++x)
        {
            const NvU32 red = width > 1 ? (255U * x) / (width - 1) : 0;
            const NvU32 blue = 255U - ((red + green) / 2U);
            const NvU32 marker = ((x / 64U) ^ (y / 64U) ^ outputIndex) & 1U;
            row[x] = 0xff000000U |
                ((marker ? red : red / 2U) << 16) |
                ((marker ? green : green / 2U) << 8) |
                blue;
        }
    }
}

HRESULT CreateSwapChainDevice(
    _In_ LUID adapterLuid,
    _Outptr_ IDXGIDevice** device) noexcept
{
    *device = nullptr;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    result = factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(&adapter));
    if (FAILED(result))
    {
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    result = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3dDevice,
        nullptr,
        &d3dContext);
    if (FAILED(result))
    {
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    result = d3dDevice.As(&dxgiDevice);
    if (SUCCEEDED(result))
    {
        *device = dxgiDevice.Detach();
    }
    return result;
}

} // namespace

namespace nv::iddcx {

Adapter::Adapter() noexcept
    : handle_(nullptr),
    outputs_{},
    outputCount_(0),
      initializationStatus_(STATUS_NOT_FOUND),
      state_(State::Stopped),
    powered_(false),
    displayReady_(0)
{
}

NTSTATUS Adapter::EnterD0(_In_ WDFDEVICE device) noexcept
{
    powered_ = true;
    if (handle_ != nullptr)
    {
        return state_ == State::Failed
            ? initializationStatus_
            : STATUS_SUCCESS;
    }

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;

    IDDCX_ADAPTER_CAPS capabilities = {};
    capabilities.Size = sizeof(capabilities);
    capabilities.Flags = IDDCX_ADAPTER_FLAGS_NONE;
    capabilities.MaxDisplayPipelineRate =
        static_cast<UINT64>(7680) * 4320 * 240;
    capabilities.MaxMonitorsSupported = MaxOutputs;
    capabilities.EndPointDiagnostics.Size =
        sizeof(capabilities.EndPointDiagnostics);
    capabilities.EndPointDiagnostics.TransmissionType =
        IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    capabilities.EndPointDiagnostics.pEndPointFriendlyName = EndpointFriendlyName;
    capabilities.EndPointDiagnostics.pEndPointManufacturerName =
        EndpointManufacturerName;
    capabilities.EndPointDiagnostics.pEndPointModelName = EndpointModelName;
    capabilities.EndPointDiagnostics.pHardwareVersion = &version;
    capabilities.EndPointDiagnostics.pFirmwareVersion = &version;
    capabilities.EndPointDiagnostics.GammaSupport =
        IDDCX_FEATURE_IMPLEMENTATION_NONE;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AdapterObjectContext);
    attributes.EvtCleanupCallback = Cleanup;

    IDARG_IN_ADAPTER_INIT input = {};
    input.WdfDevice = device;
    input.pCaps = &capabilities;
    input.ObjectAttributes = &attributes;

    IDARG_OUT_ADAPTER_INIT output = {};
    const NTSTATUS status = IddCxAdapterInitAsync(&input, &output);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxAdapterInitAsync: %!STATUS! adapter=%p",
        status,
        output.AdapterObject);
    if (!NT_SUCCESS(status))
    {
        powered_ = false;
        initializationStatus_ = status;
        state_ = State::Failed;
        return status;
    }

    handle_ = output.AdapterObject;
    GetAdapterObjectContext(handle_)->owner = this;
    initializationStatus_ = STATUS_PENDING;
    state_ = State::Initializing;
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::NotifyDisplayReady() noexcept
{
    InterlockedExchange(&displayReady_, 1);
    return TryAnnounceMonitors();
}

void Adapter::ExitD0() noexcept
{
    powered_ = false;
}

void Adapter::Reset() noexcept
{
    powered_ = false;
    ResetOutputs();
    if (handle_ != nullptr)
    {
        AdapterObjectContext* context = GetAdapterObjectContext(handle_);
        context->owner = nullptr;
        WdfObjectDelete(handle_);
        handle_ = nullptr;
    }

    initializationStatus_ = STATUS_NOT_FOUND;
    state_ = State::Stopped;
    InterlockedExchange(&displayReady_, 0);
}

NTSTATUS Adapter::CompleteInitialization(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* arguments) noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Initializing)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    initializationStatus_ = arguments->AdapterInitStatus;
    TraceEvents(
        NT_SUCCESS(initializationStatus_)
            ? TRACE_LEVEL_INFORMATION
            : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "AdapterInitFinished: %!STATUS!",
        initializationStatus_);
    if (!NT_SUCCESS(initializationStatus_))
    {
        state_ = State::Failed;
        return STATUS_SUCCESS;
    }

    state_ = State::Ready;
    initializationStatus_ = TryAnnounceMonitors();
    if (!NT_SUCCESS(initializationStatus_))
    {
        state_ = State::Failed;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::TryAnnounceMonitors() noexcept
{
    if (state_ != State::Ready ||
        !powered_ ||
        InterlockedCompareExchange(&displayReady_, 0, 0) == 0)
    {
        return STATUS_SUCCESS;
    }
    if (outputCount_ != 0)
    {
        return STATUS_SUCCESS;
    }

    NvU32 connectedCount = 0;
    const NV_STATUS countStatus =
        NvKmsWindowsGetConnectedDisplayCount(&connectedCount);
    if (countStatus != NV_OK)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (connectedCount > MaxOutputs)
    {
        connectedCount = MaxOutputs;
    }

    for (NvU32 index = 0; index < connectedCount; ++index)
    {
        auto* const output = static_cast<OutputState*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(OutputState)));
        if (output == nullptr)
        {
            ResetOutputs();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        output->owner = this;
        InitializeSRWLock(&output->swapchainLock);
        const NTSTATUS stateStatus = GetDisplayState(index, &output->displayState);
        if (!NT_SUCCESS(stateStatus))
        {
            HeapFree(GetProcessHeap(), 0, output);
            ResetOutputs();
            return stateStatus;
        }

        outputs_[outputCount_++] = output;
        const NTSTATUS arrivalStatus = AnnounceMonitor(output);
        if (!NT_SUCCESS(arrivalStatus))
        {
            ResetOutputs();
            return arrivalStatus;
        }
    }

    ModePath paths[MaxOutputs] = {};
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        OutputState* const output = outputs_[index];
        const NvU32 preferredModeIndex =
            output->displayState.preferredModeIndex < output->displayState.modeCount
                ? output->displayState.preferredModeIndex
                : 0;
        paths[index].monitor = output->monitor;
        paths[index].flags = IDDCX_PATH_FLAGS_ACTIVE;
        FillSignalInfo(
            &paths[index].signalInfo,
            output->displayState.modes[preferredModeIndex],
            false);
    }

    const NTSTATUS restoreStatus = CommitDisplayModes(paths, outputCount_);
    TraceEvents(
        NT_SUCCESS(restoreStatus) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "Preferred mode restore: %!STATUS! outputs=%u",
        restoreStatus,
        outputCount_);
    if (!NT_SUCCESS(restoreStatus))
    {
        ResetOutputs();
        return restoreStatus;
    }

    return STATUS_SUCCESS;
}

NTSTATUS Adapter::AnnounceMonitor(_Inout_ OutputState* output) noexcept
{
    NV_KMS_WINDOWS_DISPLAY_STATE& displayState = output->displayState;

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = GetOutputTechnology(displayState.connectorType);
    monitorInfo.ConnectorIndex = displayState.connectorIndex;
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = displayState.edidBytes;
    monitorInfo.MonitorDescription.pData = displayState.edid;
    monitorInfo.MonitorContainerId = {
        0x1500c36c ^ displayState.displayHandle, 0x73e5, 0x4bd2,
        { 0x93, 0xa7, 0xe6, 0x8c, 0x59, 0xd4, 0xe7, 0x9c }
    };

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorObjectContext);

    IDARG_IN_MONITORCREATE input = {};
    input.ObjectAttributes = &attributes;
    input.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE createOutput = {};
    NTSTATUS status = IddCxMonitorCreate(handle_, &input, &createOutput);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorCreate: %!STATUS! monitor=%p",
        status,
        createOutput.MonitorObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    output->monitor = createOutput.MonitorObject;
    GetMonitorObjectContext(output->monitor)->owner = this;
    GetMonitorObjectContext(output->monitor)->output = output;
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "NVKMS monitor: connector=%u physical=%u EDID=%u modes=%u heads=%u layers=%u multiLayer=%u",
        displayState.connectorType,
        displayState.connectorIndex,
        displayState.edidBytes,
        displayState.modeCount,
        displayState.headCount,
        displayState.maxLayersPerHead,
        displayState.supportsMultiLayerFlip);
    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(output->monitor, &arrival);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorArrival: %!STATUS! target=%u",
        status,
        arrival.OsTargetId);
    if (!NT_SUCCESS(status))
    {
        GetMonitorObjectContext(output->monitor)->owner = nullptr;
        GetMonitorObjectContext(output->monitor)->output = nullptr;
        WdfObjectDelete(output->monitor);
        output->monitor = nullptr;
    }

    return status;
}

NTSTATUS Adapter::AssignSwapChain(
    _Inout_ OutputState* output,
    _In_ IDDCX_MONITOR monitor,
    _In_ const IDARG_IN_SETSWAPCHAIN* arguments) noexcept
{
    if (output == nullptr || monitor != output->monitor ||
        arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    StopSwapChain(output);

    IDXGIDevice* device = nullptr;
    HRESULT result = CreateSwapChainDevice(arguments->RenderAdapterLuid, &device);
    if (FAILED(result))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: create D3D device failed 0x%08X",
            result);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = device;
    result = IddCxSwapChainSetDevice(arguments->hSwapChain, &setDevice);
    if (FAILED(result))
    {
        device->Release();
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: set D3D device failed 0x%08X",
            result);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr)
    {
        device->Release();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    AcquireSRWLockExclusive(&output->swapchainLock);
    output->swapchain = arguments->hSwapChain;
    output->swapchainDevice = device;
    output->swapchainStopEvent = stopEvent;
    output->nextSurfaceEvent = arguments->hNextSurfaceAvailable;
    ReleaseSRWLockExclusive(&output->swapchainLock);

    output->swapchainThread =
        CreateThread(nullptr, 0, SwapChainThread, output, 0, nullptr);
    if (output->swapchainThread == nullptr)
    {
        AcquireSRWLockExclusive(&output->swapchainLock);
        output->swapchain = nullptr;
        output->swapchainDevice = nullptr;
        output->swapchainStopEvent = nullptr;
        output->nextSurfaceEvent = nullptr;
        ReleaseSRWLockExclusive(&output->swapchainLock);
        CloseHandle(stopEvent);
        device->Release();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AssignSwapChain: processing swapchain=%p",
        output->swapchain);
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::UnassignSwapChain(
    _Inout_ OutputState* output,
    _In_ IDDCX_MONITOR monitor) noexcept
{
    if (output == nullptr || monitor != output->monitor)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    StopSwapChain(output);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "UnassignSwapChain");
    return STATUS_SUCCESS;
}

DWORD WINAPI Adapter::SwapChainThread(_In_ void* context)
{
    auto* const output = static_cast<OutputState*>(context);
    output->owner->ProcessSwapChain(output);
    return 0;
}

void Adapter::ProcessSwapChain(_Inout_ OutputState* output) noexcept
{
    AcquireSRWLockShared(&output->swapchainLock);
    IDDCX_SWAPCHAIN swapchain = output->swapchain;
    HANDLE stopEvent = output->swapchainStopEvent;
    HANDLE nextSurfaceEvent = output->nextSurfaceEvent;
    ReleaseSRWLockShared(&output->swapchainLock);

    const HANDLE waitHandles[] = { nextSurfaceEvent, stopEvent };
    HRESULT result = S_OK;
    ULONG frameCount = 0;
    bool processingFailed = false;
    for (;;)
    {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        result = IddCxSwapChainReleaseAndAcquireBuffer(swapchain, &buffer);
        if (result == E_PENDING)
        {
            const DWORD waitResult = WaitForMultipleObjects(
                ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0)
            {
                continue;
            }
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                break;
            }

            result = HRESULT_FROM_WIN32(GetLastError());
            processingFailed = true;
            break;
        }
        if (FAILED(result))
        {
            processingFailed = true;
            break;
        }

        if (buffer.MetaData.pSurface != nullptr)
        {
            buffer.MetaData.pSurface->Release();
        }

        result = IddCxSwapChainFinishedProcessingFrame(swapchain);
        if (FAILED(result))
        {
            processingFailed = true;
            break;
        }
        ++frameCount;
    }

    TraceEvents(
        processingFailed ? TRACE_LEVEL_ERROR : TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "Swap-chain processor stopped: result=0x%08X frames=%u",
        result,
        frameCount);

    if (processingFailed)
    {
        bool deleteSwapChain = false;
        AcquireSRWLockExclusive(&output->swapchainLock);
        if (output->swapchain == swapchain)
        {
            output->swapchain = nullptr;
            deleteSwapChain = true;
        }
        ReleaseSRWLockExclusive(&output->swapchainLock);
        if (deleteSwapChain)
        {
            WdfObjectDelete(swapchain);
        }
    }
}

void Adapter::StopSwapChain(_Inout_ OutputState* output) noexcept
{
    if (output->swapchainStopEvent != nullptr)
    {
        SetEvent(output->swapchainStopEvent);
    }
    if (output->swapchainThread != nullptr)
    {
        WaitForSingleObject(output->swapchainThread, INFINITE);
        CloseHandle(output->swapchainThread);
        output->swapchainThread = nullptr;
    }

    AcquireSRWLockExclusive(&output->swapchainLock);
    IDDCX_SWAPCHAIN swapchain = output->swapchain;
    output->swapchain = nullptr;
    IDXGIDevice* device = output->swapchainDevice;
    output->swapchainDevice = nullptr;
    HANDLE stopEvent = output->swapchainStopEvent;
    output->swapchainStopEvent = nullptr;
    output->nextSurfaceEvent = nullptr;
    ReleaseSRWLockExclusive(&output->swapchainLock);

    if (swapchain != nullptr)
    {
        WdfObjectDelete(swapchain);
    }
    if (device != nullptr)
    {
        device->Release();
    }
    if (stopEvent != nullptr)
    {
        CloseHandle(stopEvent);
    }
}

void Adapter::ResetOutputs() noexcept
{
    if (outputCount_ != 0)
    {
        NvKmsWindowsSetDisplayModes(nullptr, 0);
    }
    while (outputCount_ != 0)
    {
        OutputState* const output = outputs_[--outputCount_];
        outputs_[outputCount_] = nullptr;
        StopSwapChain(output);
        NvKmsWindowsDestroyScanoutSurface(output->scanoutSurface);
        output->scanoutSurface = nullptr;
        output->scanoutPixels = nullptr;
        output->scanoutPitch = 0;
        if (output->monitor != nullptr)
        {
            MonitorObjectContext* const context =
                GetMonitorObjectContext(output->monitor);
            context->owner = nullptr;
            context->output = nullptr;
            WdfObjectDelete(output->monitor);
            output->monitor = nullptr;
        }
        HeapFree(GetProcessHeap(), 0, output);
    }
}

NTSTATUS Adapter::CommitModes(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_COMMITMODES* arguments) noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!powered_)
    {
        return STATUS_DEVICE_POWER_FAILURE;
    }
    if (arguments->PathCount > MaxOutputs ||
        (arguments->PathCount != 0 && arguments->pPaths == nullptr))
    {
        return STATUS_INVALID_PARAMETER;
    }

    ModePath paths[MaxOutputs] = {};
    for (UINT index = 0; index < arguments->PathCount; ++index)
    {
        paths[index].monitor = arguments->pPaths[index].MonitorObject;
        paths[index].flags = arguments->pPaths[index].Flags;
        paths[index].signalInfo = arguments->pPaths[index].TargetVideoSignalInfo;
    }
    return CommitDisplayModes(paths, arguments->PathCount);
}

NTSTATUS Adapter::CommitModes2(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_COMMITMODES2* arguments) noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!powered_)
    {
        return STATUS_DEVICE_POWER_FAILURE;
    }
    if (arguments->PathCount > MaxOutputs ||
        (arguments->PathCount != 0 && arguments->pPaths == nullptr))
    {
        return STATUS_INVALID_PARAMETER;
    }

    ModePath paths[MaxOutputs] = {};
    for (UINT index = 0; index < arguments->PathCount; ++index)
    {
        paths[index].monitor = arguments->pPaths[index].MonitorObject;
        paths[index].flags = arguments->pPaths[index].Flags;
        paths[index].signalInfo = arguments->pPaths[index].TargetVideoSignalInfo;
    }
    return CommitDisplayModes(paths, arguments->PathCount);
}

NTSTATUS Adapter::CommitDisplayModes(
    _In_reads_(pathCount) const ModePath* paths,
    _In_ UINT pathCount) noexcept
{
    bool active[MaxOutputs] = {};
    NvU32 modeIndexes[MaxOutputs] = {};
    NV_KMS_WINDOWS_SURFACE* surfaces[MaxOutputs] = {};
    NV_KMS_WINDOWS_SURFACE* newSurfaces[MaxOutputs] = {};
    void* newPixels[MaxOutputs] = {};
    NvU32 newPitches[MaxOutputs] = {};
    NV_KMS_WINDOWS_MODESET_OUTPUT requests[MaxOutputs] = {};
    NvU32 requestOutputs[MaxOutputs] = {};
    NvU32 requestCount = 0;

    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        active[index] = outputs_[index]->active;
        modeIndexes[index] = outputs_[index]->activeModeIndex;
        surfaces[index] = outputs_[index]->scanoutSurface;
    }

    NTSTATUS status = STATUS_SUCCESS;
    for (UINT pathIndex = 0; pathIndex < pathCount; ++pathIndex)
    {
        MonitorObjectContext* const context =
            GetMonitorObjectContext(paths[pathIndex].monitor);
        if (context->owner != this || context->output == nullptr)
        {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup;
        }

        NvU32 outputIndex = 0;
        while (outputIndex < outputCount_ &&
               outputs_[outputIndex] != context->output)
        {
            ++outputIndex;
        }
        if (outputIndex == outputCount_)
        {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup;
        }

        active[outputIndex] =
            (paths[pathIndex].flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        if (!active[outputIndex])
        {
            surfaces[outputIndex] = nullptr;
            continue;
        }

        const NV_KMS_WINDOWS_MODE* const mode = FindDisplayMode(
            context->output->displayState,
            paths[pathIndex].signalInfo);
        if (mode == nullptr)
        {
            status = STATUS_GRAPHICS_MODE_NOT_PINNED;
            goto cleanup;
        }

        modeIndexes[outputIndex] = mode->kmsModeIndex;
        const bool replaceSurface = context->output->scanoutSurface == nullptr ||
            context->output->activeModeIndex != mode->kmsModeIndex;
        if (replaceSurface)
        {
            const NV_STATUS createStatus = NvKmsWindowsCreateScanoutSurface(
                mode->hVisible,
                mode->vVisible,
                &newSurfaces[outputIndex],
                &newPixels[outputIndex],
                &newPitches[outputIndex]);
            if (createStatus != NV_OK)
            {
                status = createStatus == NV_ERR_NO_MEMORY ?
                    STATUS_INSUFFICIENT_RESOURCES : STATUS_DEVICE_NOT_READY;
                goto cleanup;
            }
            FillGradient(
                newPixels[outputIndex],
                newPitches[outputIndex],
                mode->hVisible,
                mode->vVisible,
                outputIndex);
            surfaces[outputIndex] = newSurfaces[outputIndex];
        }
    }

    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        if (!active[index])
        {
            continue;
        }
        if (surfaces[index] == nullptr)
        {
            status = STATUS_INVALID_DEVICE_STATE;
            goto cleanup;
        }

        requests[requestCount].displayHandle =
            outputs_[index]->displayState.displayHandle;
        requests[requestCount].modeIndex = modeIndexes[index];
        requests[requestCount].surface = surfaces[index];
        requestOutputs[requestCount] = index;
        ++requestCount;
    }

    {
        const NV_STATUS modesetStatus =
            NvKmsWindowsSetDisplayModes(requests, requestCount);
        TraceEvents(
            modesetStatus == NV_OK ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "NvKmsWindowsSetDisplayModes: 0x%08X outputs=%u",
            modesetStatus,
            requestCount);
        if (modesetStatus != NV_OK)
        {
            status = modesetStatus == NV_ERR_NOT_SUPPORTED ?
                STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY : STATUS_DEVICE_NOT_READY;
            goto cleanup;
        }
    }

    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        OutputState* const output = outputs_[index];
        if (newSurfaces[index] != nullptr)
        {
            NvKmsWindowsDestroyScanoutSurface(output->scanoutSurface);
            output->scanoutSurface = newSurfaces[index];
            output->scanoutPixels = newPixels[index];
            output->scanoutPitch = newPitches[index];
            newSurfaces[index] = nullptr;
        }
        else if (!active[index] && output->scanoutSurface != nullptr)
        {
            NvKmsWindowsDestroyScanoutSurface(output->scanoutSurface);
            output->scanoutSurface = nullptr;
            output->scanoutPixels = nullptr;
            output->scanoutPitch = 0;
        }
        output->active = active[index];
        output->activeModeIndex = modeIndexes[index];
    }
    for (NvU32 requestIndex = 0; requestIndex < requestCount; ++requestIndex)
    {
        OutputState* const output = outputs_[requestOutputs[requestIndex]];
        output->assignedHead = requests[requestIndex].assignedHead;
    }

cleanup:
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        NvKmsWindowsDestroyScanoutSurface(newSurfaces[index]);
    }
    return status;
}

_Use_decl_annotations_
void Adapter::Cleanup(WDFOBJECT adapterObject)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapterObject);
    if (context->owner != nullptr)
    {
        context->owner->ResetOutputs();
        context->owner->handle_ = nullptr;
        context->owner->initializationStatus_ = STATUS_NOT_FOUND;
        context->owner->state_ = State::Stopped;
        context->owner->powered_ = false;
        context->owner = nullptr;
    }
}

_Use_decl_annotations_
NTSTATUS AdapterInitFinished(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_ADAPTER_INIT_FINISHED* arguments)
{
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AdapterInitFinished entry: adapter=%p arguments=%p status=%!STATUS!",
        adapter,
        arguments,
        arguments != nullptr
            ? arguments->AdapterInitStatus
            : STATUS_INVALID_PARAMETER);
    AdapterObjectContext* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CompleteInitialization(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterCommitModes(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES* arguments)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CommitModes(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterCommitModes2(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES2* arguments)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CommitModes2(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterQueryTargetInfo(
    IDDCX_ADAPTER adapter,
    IDARG_IN_QUERYTARGET_INFO* input,
    IDARG_OUT_QUERYTARGET_INFO* output)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapter);
    if (context->owner == nullptr || input->ConnectorIndex >= Adapter::MaxOutputs)
    {
        return STATUS_INVALID_PARAMETER;
    }

    output->TargetCaps = IDDCX_TARGET_CAPS_NONE;
    output->DitheringSupport = {};
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AdapterQueryTargetInfo: connector=%u SDR only",
        input->ConnectorIndex);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* input,
    IDARG_OUT_PARSEMONITORDESCRIPTION* output)
{
    NV_KMS_WINDOWS_DISPLAY_STATE displayState = {};
    const NTSTATUS status =
        GetDisplayState(input->MonitorDescription, &displayState);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    return FillMonitorModeBuffer(
        displayState,
        input->MonitorModeBufferInputCount,
        input->pMonitorModes,
        IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
        &output->MonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

_Use_decl_annotations_
NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* input,
    IDARG_OUT_PARSEMONITORDESCRIPTION* output)
{
    NV_KMS_WINDOWS_DISPLAY_STATE displayState = {};
    const NTSTATUS status =
        GetDisplayState(input->MonitorDescription, &displayState);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    return FillMonitorMode2Buffer(
        displayState,
        input->MonitorModeBufferInputCount,
        input->pMonitorModes,
        &output->MonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

_Use_decl_annotations_
NTSTATUS GetDefaultDescriptionModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* input,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output)
{
    MonitorObjectContext* const context = GetMonitorObjectContext(monitor);
    if (context->output == nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    return FillMonitorModeBuffer(
        context->output->displayState,
        input->DefaultMonitorModeBufferInputCount,
        input->pDefaultMonitorModes,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
        &output->DefaultMonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    MonitorObjectContext* const context = GetMonitorObjectContext(monitor);
    if (context->output == nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    const NV_KMS_WINDOWS_DISPLAY_STATE& displayState =
        context->output->displayState;

    output->TargetModeBufferOutputCount = displayState.modeCount;
    if (input->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (input->TargetModeBufferInputCount < displayState.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < displayState.modeCount; ++index)
    {
        input->pTargetModes[index] = CreateTargetMode(displayState.modes[index]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes2(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES2* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    MonitorObjectContext* const context = GetMonitorObjectContext(monitor);
    if (context->output == nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    const NV_KMS_WINDOWS_DISPLAY_STATE& displayState =
        context->output->displayState;

    output->TargetModeBufferOutputCount = displayState.modeCount;
    if (input->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (input->TargetModeBufferInputCount < displayState.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < displayState.modeCount; ++index)
    {
        input->pTargetModes[index] = CreateTargetMode2(displayState.modes[index]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AssignSwapChain(
    IDDCX_MONITOR monitor,
    const IDARG_IN_SETSWAPCHAIN* input)
{
    MonitorObjectContext* context = GetMonitorObjectContext(monitor);
    return context->owner == nullptr || context->output == nullptr
        ? STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN
        : context->owner->AssignSwapChain(context->output, monitor, input);
}

_Use_decl_annotations_
NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor)
{
    MonitorObjectContext* context = GetMonitorObjectContext(monitor);
    return context->owner == nullptr || context->output == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->UnassignSwapChain(context->output, monitor);
}

} // namespace nv::iddcx