#include "adapter.hpp"
#include "trace.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "adapter.tmh"

namespace {

constexpr wchar_t EndpointFriendlyName[] = L"NVIDIA Indirect Display Adapter";
constexpr wchar_t EndpointManufacturerName[] = L"NVIDIA";
constexpr wchar_t EndpointModelName[] = L"Open GPU Kernel Modules";
constexpr UINT MonitorWidth = 1920;
constexpr UINT MonitorHeight = 1080;
constexpr UINT MonitorRefreshRate = 60;

void FillSignalInfo(
    _Out_ DISPLAYCONFIG_VIDEO_SIGNAL_INFO* signalInfo,
    _In_ bool monitorMode) noexcept
{
    signalInfo->pixelRate =
        static_cast<UINT64>(MonitorWidth) * MonitorHeight * MonitorRefreshRate;
    signalInfo->hSyncFreq.Numerator = MonitorHeight * MonitorRefreshRate;
    signalInfo->hSyncFreq.Denominator = 1;
    signalInfo->vSyncFreq.Numerator = MonitorRefreshRate;
    signalInfo->vSyncFreq.Denominator = 1;
    signalInfo->activeSize.cx = MonitorWidth;
    signalInfo->activeSize.cy = MonitorHeight;
    signalInfo->totalSize = signalInfo->activeSize;
    signalInfo->videoStandard = 255;
    signalInfo->scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    signalInfo->AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
}

IDDCX_MONITOR_MODE CreateMonitorMode() noexcept
{
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(&mode.MonitorVideoSignalInfo, true);
    return mode;
}

IDDCX_TARGET_MODE CreateTargetMode() noexcept
{
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(&mode.TargetVideoSignalInfo.targetVideoSignalInfo, false);
    return mode;
}

IDDCX_TARGET_MODE2 CreateTargetMode2() noexcept
{
    IDDCX_TARGET_MODE2 mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(&mode.TargetVideoSignalInfo.targetVideoSignalInfo, false);
    mode.RequiredBandwidth = mode.TargetVideoSignalInfo
        .targetVideoSignalInfo.pixelRate;
    mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
    return mode;
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
    monitor_(nullptr),
    swapchain_(nullptr),
            swapchainDevice_(nullptr),
            swapchainThread_(nullptr),
            swapchainStopEvent_(nullptr),
            nextSurfaceEvent_(nullptr),
      initializationStatus_(STATUS_NOT_FOUND),
      state_(State::Stopped),
      powered_(false)
{
        InitializeSRWLock(&swapchainLock_);
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
        static_cast<UINT64>(MonitorWidth) * MonitorHeight * MonitorRefreshRate;
    capabilities.MaxMonitorsSupported = 1;
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

void Adapter::ExitD0() noexcept
{
    powered_ = false;
}

void Adapter::Reset() noexcept
{
    powered_ = false;
    StopSwapChain();

    if (monitor_ != nullptr)
    {
        GetMonitorObjectContext(monitor_)->owner = nullptr;
    }
    monitor_ = nullptr;
    if (handle_ != nullptr)
    {
        AdapterObjectContext* context = GetAdapterObjectContext(handle_);
        context->owner = nullptr;
        WdfObjectDelete(handle_);
        handle_ = nullptr;
    }

    initializationStatus_ = STATUS_NOT_FOUND;
    state_ = State::Stopped;
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

    initializationStatus_ = AnnounceMonitor();
    state_ = NT_SUCCESS(initializationStatus_) ? State::Ready : State::Failed;
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::AnnounceMonitor() noexcept
{
    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = 0;
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorContainerId = {
        0x1500c36c, 0x73e5, 0x4bd2,
        { 0x93, 0xa7, 0xe6, 0x8c, 0x59, 0xd4, 0xe7, 0x9c }
    };

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorObjectContext);

    IDARG_IN_MONITORCREATE input = {};
    input.ObjectAttributes = &attributes;
    input.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE output = {};
    NTSTATUS status = IddCxMonitorCreate(handle_, &input, &output);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorCreate: %!STATUS! monitor=%p",
        status,
        output.MonitorObject);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    monitor_ = output.MonitorObject;
    GetMonitorObjectContext(monitor_)->owner = this;
    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(monitor_, &arrival);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorArrival: %!STATUS! target=%u",
        status,
        arrival.OsTargetId);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(monitor_);
        monitor_ = nullptr;
    }

    return status;
}

NTSTATUS Adapter::AssignSwapChain(
    _In_ IDDCX_MONITOR monitor,
    _In_ const IDARG_IN_SETSWAPCHAIN* arguments) noexcept
{
    if (monitor != monitor_ || arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    StopSwapChain();

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

    AcquireSRWLockExclusive(&swapchainLock_);
    swapchain_ = arguments->hSwapChain;
    swapchainDevice_ = device;
    swapchainStopEvent_ = stopEvent;
    nextSurfaceEvent_ = arguments->hNextSurfaceAvailable;
    ReleaseSRWLockExclusive(&swapchainLock_);

    swapchainThread_ = CreateThread(nullptr, 0, SwapChainThread, this, 0, nullptr);
    if (swapchainThread_ == nullptr)
    {
        AcquireSRWLockExclusive(&swapchainLock_);
        swapchain_ = nullptr;
        swapchainDevice_ = nullptr;
        swapchainStopEvent_ = nullptr;
        nextSurfaceEvent_ = nullptr;
        ReleaseSRWLockExclusive(&swapchainLock_);
        CloseHandle(stopEvent);
        device->Release();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AssignSwapChain: processing swapchain=%p",
        swapchain_);
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::UnassignSwapChain(_In_ IDDCX_MONITOR monitor) noexcept
{
    if (monitor != monitor_)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    StopSwapChain();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "UnassignSwapChain");
    return STATUS_SUCCESS;
}

DWORD WINAPI Adapter::SwapChainThread(_In_ void* context)
{
    static_cast<Adapter*>(context)->ProcessSwapChain();
    return 0;
}

void Adapter::ProcessSwapChain() noexcept
{
    AcquireSRWLockShared(&swapchainLock_);
    IDDCX_SWAPCHAIN swapchain = swapchain_;
    HANDLE stopEvent = swapchainStopEvent_;
    HANDLE nextSurfaceEvent = nextSurfaceEvent_;
    ReleaseSRWLockShared(&swapchainLock_);

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
        AcquireSRWLockExclusive(&swapchainLock_);
        if (swapchain_ == swapchain)
        {
            swapchain_ = nullptr;
            deleteSwapChain = true;
        }
        ReleaseSRWLockExclusive(&swapchainLock_);
        if (deleteSwapChain)
        {
            WdfObjectDelete(swapchain);
        }
    }
}

void Adapter::StopSwapChain() noexcept
{
    if (swapchainStopEvent_ != nullptr)
    {
        SetEvent(swapchainStopEvent_);
    }
    if (swapchainThread_ != nullptr)
    {
        WaitForSingleObject(swapchainThread_, INFINITE);
        CloseHandle(swapchainThread_);
        swapchainThread_ = nullptr;
    }

    AcquireSRWLockExclusive(&swapchainLock_);
    IDDCX_SWAPCHAIN swapchain = swapchain_;
    swapchain_ = nullptr;
    IDXGIDevice* device = swapchainDevice_;
    swapchainDevice_ = nullptr;
    HANDLE stopEvent = swapchainStopEvent_;
    swapchainStopEvent_ = nullptr;
    nextSurfaceEvent_ = nullptr;
    ReleaseSRWLockExclusive(&swapchainLock_);

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

NTSTATUS Adapter::CommitModes(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_COMMITMODES* arguments) const noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    return powered_ ? STATUS_SUCCESS : STATUS_DEVICE_POWER_FAILURE;
}

NTSTATUS Adapter::CommitModes2(
    _In_ IDDCX_ADAPTER adapter,
    _In_ const IDARG_IN_COMMITMODES2* arguments) const noexcept
{
    if (adapter != handle_ || arguments == nullptr || state_ != State::Ready)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    return powered_ ? STATUS_SUCCESS : STATUS_DEVICE_POWER_FAILURE;
}

_Use_decl_annotations_
void Adapter::Cleanup(WDFOBJECT adapterObject)
{
    AdapterObjectContext* context = GetAdapterObjectContext(adapterObject);
    if (context->owner != nullptr)
    {
        context->owner->handle_ = nullptr;
        context->owner->monitor_ = nullptr;
        context->owner->swapchain_ = nullptr;
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
    if (context->owner == nullptr || input->ConnectorIndex != 0)
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
    UNREFERENCED_PARAMETER(input);
    output->MonitorModeBufferOutputCount = 0;
    output->PreferredMonitorModeIdx = NO_PREFERRED_MODE;
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* input,
    IDARG_OUT_PARSEMONITORDESCRIPTION* output)
{
    UNREFERENCED_PARAMETER(input);
    output->MonitorModeBufferOutputCount = 0;
    output->PreferredMonitorModeIdx = NO_PREFERRED_MODE;
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS GetDefaultDescriptionModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* input,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output)
{
    UNREFERENCED_PARAMETER(monitor);
    output->DefaultMonitorModeBufferOutputCount = 1;
    output->PreferredMonitorModeIdx = 0;

    if (input->DefaultMonitorModeBufferInputCount == 0)
    {
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "GetDefaultDescriptionModes: count=1");
        return STATUS_SUCCESS;
    }

    if (input->pDefaultMonitorModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    input->pDefaultMonitorModes[0] = CreateMonitorMode();
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "GetDefaultDescriptionModes: 1920x1080@60");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    UNREFERENCED_PARAMETER(monitor);
    output->TargetModeBufferOutputCount = 1;
    if (input->TargetModeBufferInputCount == 0)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "QueryTargetModes: count=1");
        return STATUS_SUCCESS;
    }

    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    input->pTargetModes[0] = CreateTargetMode();
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "QueryTargetModes: 1920x1080@60");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes2(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES2* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    UNREFERENCED_PARAMETER(monitor);
    output->TargetModeBufferOutputCount = 1;
    if (input->TargetModeBufferInputCount == 0)
    {
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "QueryTargetModes2: count=1");
        return STATUS_SUCCESS;
    }

    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    input->pTargetModes[0] = CreateTargetMode2();
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "QueryTargetModes2: 1920x1080@60 RGB 8 bpc");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AssignSwapChain(
    IDDCX_MONITOR monitor,
    const IDARG_IN_SETSWAPCHAIN* input)
{
    MonitorObjectContext* context = GetMonitorObjectContext(monitor);
    return context->owner == nullptr
        ? STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN
        : context->owner->AssignSwapChain(monitor, input);
}

_Use_decl_annotations_
NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor)
{
    MonitorObjectContext* context = GetMonitorObjectContext(monitor);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->UnassignSwapChain(monitor);
}

} // namespace nv::iddcx