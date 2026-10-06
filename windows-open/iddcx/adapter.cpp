#include "adapter.hpp"
#include "display_modes.hpp"
#include "driver_parameters.hpp"
#include "output.hpp"
#include "trace.h"

#include "adapter.tmh"

namespace {
constexpr wchar_t EndpointFriendlyName[] = L"NVIDIA Indirect Display Adapter";
constexpr wchar_t EndpointManufacturerName[] = L"NVIDIA";
constexpr wchar_t EndpointModelName[] = L"Open GPU Kernel Modules";

bool GetRgbBitsPerComponent(
    const IDDCX_WIRE_BITS_PER_COMPONENT& bits,
    NvU8* colorBpc) noexcept
{
    if (bits.YCbCr444 != IDDCX_BITS_PER_COMPONENT_NONE ||
        bits.YCbCr422 != IDDCX_BITS_PER_COMPONENT_NONE ||
        bits.YCbCr420 != IDDCX_BITS_PER_COMPONENT_NONE)
    {
        return false;
    }
    switch (bits.Rgb)
    {
    case IDDCX_BITS_PER_COMPONENT_8:
        *colorBpc = 8;
        return true;
    case IDDCX_BITS_PER_COMPONENT_10:
        *colorBpc = 10;
        return true;
    case IDDCX_BITS_PER_COMPONENT_12:
        *colorBpc = 12;
        return true;
    default:
        return false;
    }
}

bool GetColorSpace(
    IDDCX_COLOR_SPACE colorSpace,
    NV_KMS_WINDOWS_COLOR_SPACE* kmsColorSpace) noexcept
{
    switch (colorSpace)
    {
    case IDDCX_COLOR_SPACE_G22_P709:
        *kmsColorSpace = NV_KMS_WINDOWS_COLOR_SPACE_SDR;
        return true;
    case IDDCX_COLOR_SPACE_G22_P709_WCG:
        *kmsColorSpace = NV_KMS_WINDOWS_COLOR_SPACE_SDR_WCG;
        return true;
    case IDDCX_COLOR_SPACE_G2084_P2020:
        *kmsColorSpace = NV_KMS_WINDOWS_COLOR_SPACE_HDR10;
        return true;
    default:
        return false;
    }
}
} // namespace

namespace nv::iddcx {

Adapter::Adapter() noexcept
    : device_(nullptr), handle_(nullptr), outputs_{}, outputCount_(0),
      initializationStatus_(STATUS_NOT_FOUND),
      state_(State::Stopped), powered_(false), displayReady_(0)
{
}

NTSTATUS Adapter::EnterD0(WDFDEVICE device) noexcept
{
    device_ = device;
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
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.Flags = IDDCX_ADAPTER_FLAGS_NONE;
    ULONG dirtyRects = 0;
    const NTSTATUS dirtyStatus = QueryDirtyRectsEnabled(&dirtyRects);
    if (!NT_SUCCESS(dirtyStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid DirtyRects parameter: status=0x%08X value=%u", dirtyStatus, dirtyRects);
        return NT_SUCCESS(dirtyStatus) ? STATUS_INVALID_PARAMETER : dirtyStatus;
    }
    if (dirtyRects != 0)
    {
        caps.Flags |= IDDCX_ADAPTER_FLAGS_PREFER_PRECISE_PRESENT_REGIONS;
    }
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2))
    {
        caps.Flags |= IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
    }
    caps.MaxDisplayPipelineRate = static_cast<UINT64>(7680) * 4320 * 240;
    caps.MaxMonitorsSupported = MaxOutputs;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = EndpointFriendlyName;
    caps.EndPointDiagnostics.pEndPointManufacturerName = EndpointManufacturerName;
    caps.EndPointDiagnostics.pEndPointModelName = EndpointModelName;
    caps.EndPointDiagnostics.pHardwareVersion = &version;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_HARDWARE;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AdapterObjectContext);
    attributes.EvtCleanupCallback = Cleanup;
    IDARG_IN_ADAPTER_INIT input = {};
    input.WdfDevice = device;
    input.pCaps = &caps;
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
        GetAdapterObjectContext(handle_)->owner = nullptr;
        WdfObjectDelete(handle_);
        handle_ = nullptr;
    }
    initializationStatus_ = STATUS_NOT_FOUND;
    state_ = State::Stopped;
    device_ = nullptr;
    InterlockedExchange(&displayReady_, 0);
}

bool Adapter::IsReady() const noexcept
{
    return state_ == State::Ready && powered_;
}

NTSTATUS Adapter::CompleteInitialization(IDDCX_ADAPTER adapter,
    const IDARG_IN_ADAPTER_INIT_FINISHED* arguments) noexcept
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
    return RefreshDisplays();
}

NTSTATUS Adapter::RefreshDisplays() noexcept
{
    if (!IsReady() || InterlockedCompareExchange(&displayReady_, 0, 0) == 0)
    {
        return STATUS_SUCCESS;
    }
    NvU32 connectedCount = 0;
    if (NvKmsWindowsGetConnectedDisplayCount(&connectedCount) != NV_OK)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (connectedCount > MaxOutputs)
    {
        connectedCount = MaxOutputs;
    }

    NV_KMS_WINDOWS_DISPLAY_STATE displays[MaxOutputs] = {};
    for (NvU32 index = 0; index < connectedCount; ++index)
    {
        const NTSTATUS status = display_modes::GetDisplayState(
            index, &displays[index]);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    bool connected[MaxOutputs] = {};
    bool topologyChanged = false;
    bool hasDeparture = false;
    for (NvU32 displayIndex = 0; displayIndex < connectedCount; ++displayIndex)
    {
        NvU32 outputIndex = 0;
        while (outputIndex < outputCount_ &&
               outputs_[outputIndex]->DisplayState().displayHandle !=
                   displays[displayIndex].displayHandle)
        {
            ++outputIndex;
        }
        if (outputIndex < outputCount_)
        {
            connected[outputIndex] = true;
            continue;
        }

        Output* output = nullptr;
        const NTSTATUS status = Output::Create(
            *this, displays[displayIndex], &output);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        connected[outputCount_] = true;
        outputs_[outputCount_++] = output;
        topologyChanged = true;
    }
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        topologyChanged = topologyChanged || !connected[index];
        hasDeparture = hasDeparture || !connected[index];
    }
    if (!topologyChanged)
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS status = STATUS_SUCCESS;
    if (hasDeparture)
    {
        ModePath paths[MaxOutputs] = {};
        for (NvU32 index = 0; index < outputCount_; ++index)
        {
            Output* const output = outputs_[index];
            paths[index].monitor = output->Monitor();
            if (!connected[index])
            {
                continue;
            }

            auto lock = output->LockFrameStateShared();
            if (!output->Active())
            {
                continue;
            }
            const auto* const mode = display_modes::FindDisplayMode(
                output->DisplayState(), output->ActiveModeIndex());
            if (mode == nullptr)
            {
                return STATUS_GRAPHICS_MODE_NOT_PINNED;
            }
            paths[index].flags = IDDCX_PATH_FLAGS_ACTIVE;
            display_modes::FillSignalInfo(
                &paths[index].signalInfo,
                *mode,
                false);
            paths[index].colorBpc = output->ColorBpc();
            paths[index].colorSpace = output->ColorSpace();
        }

        status = CommitDisplayModes(paths, outputCount_);
        TraceEvents(
            NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "Display departure modeset: %!STATUS! outputs=%u connected=%u",
            status,
            outputCount_,
            connectedCount);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    for (NvU32 index = outputCount_; index-- > 0;)
    {
        if (connected[index])
        {
            continue;
        }
        Output* const output = outputs_[index];
        const NTSTATUS departureStatus = IddCxMonitorDeparture(output->Monitor());
        TraceEvents(
            NT_SUCCESS(departureStatus)
                ? TRACE_LEVEL_INFORMATION
                : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "IddCxMonitorDeparture: %!STATUS! display=0x%08X",
            departureStatus,
            output->DisplayState().displayHandle);
        if (!NT_SUCCESS(departureStatus))
        {
            status = departureStatus;
            continue;
        }
        OnOutputDestroyed(output);
        WdfObjectDelete(output->Monitor());
    }
    return status;
}

void Adapter::ResetOutputs() noexcept
{
    if (outputCount_ != 0)
    {
        NvKmsWindowsSetDisplayModes(nullptr, 0);
    }
    // Monitor deletion runs Output cleanup, including swapchain stop/join.
    while (outputCount_ != 0)
    {
        Output* output = outputs_[--outputCount_];
        outputs_[outputCount_] = nullptr;
        WdfObjectDelete(output->Monitor());
    }
}

void Adapter::OnOutputDestroyed(Output* output) noexcept
{
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        if (outputs_[index] != output)
        {
            continue;
        }
        for (NvU32 move = index + 1; move < outputCount_; ++move)
        {
            outputs_[move - 1] = outputs_[move];
        }
        outputs_[--outputCount_] = nullptr;
        return;
    }
}

NTSTATUS Adapter::CommitModes(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES* arguments) noexcept
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
        paths[index].colorBpc = 8;
        paths[index].colorSpace = NV_KMS_WINDOWS_COLOR_SPACE_SDR;
    }
    return CommitDisplayModes(paths, arguments->PathCount);
}

NTSTATUS Adapter::CommitModes2(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES2* arguments) noexcept
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
        if ((paths[index].flags & IDDCX_PATH_FLAGS_ACTIVE) != 0 &&
            (!GetRgbBitsPerComponent(
                arguments->pPaths[index].WireFormatInfo.BitsPerComponent,
                &paths[index].colorBpc) ||
             !GetColorSpace(
                arguments->pPaths[index].WireFormatInfo.ColorSpace,
                &paths[index].colorSpace)))
        {
            return STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
        }
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "CommitModes2[%u]: active=%u colorSpace=%u RGB=%u bpc",
            index,
            (paths[index].flags & IDDCX_PATH_FLAGS_ACTIVE) != 0,
            arguments->pPaths[index].WireFormatInfo.ColorSpace,
            paths[index].colorBpc);
    }
    return CommitDisplayModes(paths, arguments->PathCount);
}

NTSTATUS Adapter::CommitDisplayModes(const ModePath* paths, UINT pathCount) noexcept
{
    bool active[MaxOutputs] = {};
    NvU32 modeIndexes[MaxOutputs] = {};
    NV_KMS_WINDOWS_SURFACE* surfaces[MaxOutputs] = {};
    NV_KMS_WINDOWS_SURFACE* newSurfaces[MaxOutputs] = {};
    void* newPixels[MaxOutputs] = {};
    NvU32 newPitches[MaxOutputs] = {};
    NvU8 colorBpc[MaxOutputs] = {};
    NV_KMS_WINDOWS_COLOR_SPACE colorSpaces[MaxOutputs] = {};
    NV_KMS_WINDOWS_SCANOUT_FORMAT scanoutFormats[MaxOutputs] = {};
    NV_KMS_WINDOWS_HDR_METADATA hdrMetadata[MaxOutputs] = {};
    bool hdrMetadataEnabled[MaxOutputs] = {};
    NV_KMS_WINDOWS_MODESET_OUTPUT requests[MaxOutputs] = {};
    NvU32 requestOutputs[MaxOutputs] = {};
    NvU32 requestCount = 0;
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        auto lock = outputs_[index]->LockFrameStateShared();
        active[index] = outputs_[index]->Active();
        modeIndexes[index] = outputs_[index]->ActiveModeIndex();
        surfaces[index] = outputs_[index]->ScanoutSurface();
        colorBpc[index] = outputs_[index]->ColorBpc();
        colorSpaces[index] = outputs_[index]->ColorSpace();
        scanoutFormats[index] = outputs_[index]->ScanoutFormat();
        hdrMetadata[index] = outputs_[index]->DefaultHdrMetadata();
        hdrMetadataEnabled[index] = outputs_[index]->HasDefaultHdrMetadata();
    }
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        outputs_[index]->PauseCursor();
        if (active[index])
        {
            outputs_[index]->QuiesceSwapChain();
        }
    }

    NTSTATUS status = STATUS_SUCCESS;
    for (UINT pathIndex = 0; pathIndex < pathCount; ++pathIndex)
    {
        Output* output = GetOutput(paths[pathIndex].monitor);
        if (output == nullptr || &output->Owner() != this)
        {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup;
        }
        NvU32 outputIndex = 0;
        while (outputIndex < outputCount_ && outputs_[outputIndex] != output)
        {
            ++outputIndex;
        }
        if (outputIndex == outputCount_)
        {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup;
        }
        active[outputIndex] = (paths[pathIndex].flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        colorBpc[outputIndex] = paths[pathIndex].colorBpc;
        colorSpaces[outputIndex] = paths[pathIndex].colorSpace;
        scanoutFormats[outputIndex] =
            paths[pathIndex].colorBpc > 8 ||
            paths[pathIndex].colorSpace != NV_KMS_WINDOWS_COLOR_SPACE_SDR
                ? NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16
                : NV_KMS_WINDOWS_SCANOUT_FORMAT_XRGB8;
        if (!active[outputIndex])
        {
            surfaces[outputIndex] = nullptr;
            continue;
        }
        const auto* mode = display_modes::FindDisplayMode(
            output->DisplayState(),
            paths[pathIndex].signalInfo);
        if (mode == nullptr)
        {
            status = STATUS_GRAPHICS_MODE_NOT_PINNED;
            goto cleanup;
        }
        modeIndexes[outputIndex] = mode->kmsModeIndex;
        bool needsScanout = false;
        {
            auto lock = output->LockFrameStateShared();
            needsScanout = output->ScanoutSurface() == nullptr ||
                output->ActiveModeIndex() != mode->kmsModeIndex ||
                output->ScanoutFormat() != scanoutFormats[outputIndex];
        }
        if (needsScanout)
        {
            status = output->PrepareScanout(
                *mode,
                outputIndex,
                scanoutFormats[outputIndex],
                &newSurfaces[outputIndex],
                &newPixels[outputIndex],
                &newPitches[outputIndex]);
            if (!NT_SUCCESS(status))
            {
                goto cleanup;
            }
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
            outputs_[index]->DisplayState().displayHandle;
        const auto* const mode = display_modes::FindDisplayMode(
            outputs_[index]->DisplayState(), modeIndexes[index]);
        if (mode == nullptr)
        {
            status = STATUS_GRAPHICS_MODE_NOT_PINNED;
            goto cleanup;
        }
        requests[requestCount].mode = *mode;
        requests[requestCount].surface = surfaces[index];
        requests[requestCount].colorBpc = colorBpc[index];
        requests[requestCount].colorSpace = colorSpaces[index];
        requests[requestCount].hdrMetadata = hdrMetadata[index];
        requests[requestCount].hdrMetadataEnabled =
            hdrMetadataEnabled[index] ? NV_TRUE : NV_FALSE;
        requestOutputs[requestCount++] = index;
    }

    // NVKMS assigns heads atomically; publish Output state only after all paths succeed.
    {
        const NV_STATUS modesetStatus =
            NvKmsWindowsSetDisplayModes(requests, requestCount);
        TraceEvents(
            modesetStatus == NV_OK
                ? TRACE_LEVEL_INFORMATION
                : TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "NvKmsWindowsSetDisplayModes: 0x%08X outputs=%u",
            modesetStatus,
            requestCount);
        if (modesetStatus != NV_OK)
        {
            status = modesetStatus == NV_ERR_NOT_SUPPORTED
                ? STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY : STATUS_DEVICE_NOT_READY;
            goto cleanup;
        }
    }
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        outputs_[index]->CommitMode(
            active[index],
            modeIndexes[index],
            newSurfaces[index],
            newPixels[index],
            newPitches[index],
            scanoutFormats[index],
            colorBpc[index],
            colorSpaces[index]);
        newSurfaces[index] = nullptr;
    }
    for (NvU32 index = 0; index < requestCount; ++index)
    {
        outputs_[requestOutputs[index]]->SetAssignedHead(requests[index].assignedHead);
    }

cleanup:
    for (NvU32 index = 0; index < outputCount_; ++index)
    {
        NvKmsWindowsDestroyScanoutSurface(newSurfaces[index]);
        if (!NT_SUCCESS(status))
        {
            outputs_[index]->ResumeCursor();
        }
    }
    return status;
}

_Use_decl_annotations_
void Adapter::Cleanup(WDFOBJECT object)
{
    auto* context = GetAdapterObjectContext(object);
    if (context->owner == nullptr)
    {
        return;
    }
    context->owner->ResetOutputs();
    context->owner->handle_ = nullptr;
    context->owner->initializationStatus_ = STATUS_NOT_FOUND;
    context->owner->state_ = State::Stopped;
    context->owner->powered_ = false;
    context->owner->device_ = nullptr;
    context->owner = nullptr;
}

_Use_decl_annotations_
NTSTATUS AdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* arguments)
{
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AdapterInitFinished entry: adapter=%p arguments=%p status=%!STATUS!",
        adapter,
        arguments,
        arguments != nullptr ? arguments->AdapterInitStatus : STATUS_INVALID_PARAMETER);
    auto* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CompleteInitialization(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* arguments)
{
    auto* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CommitModes(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterCommitModes2(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES2* arguments)
{
    auto* context = GetAdapterObjectContext(adapter);
    return context->owner == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->owner->CommitModes2(adapter, arguments);
}

_Use_decl_annotations_
NTSTATUS AdapterQueryTargetInfo(IDDCX_ADAPTER adapter, IDARG_IN_QUERYTARGET_INFO* input,
    IDARG_OUT_QUERYTARGET_INFO* output)
{
    auto* context = GetAdapterObjectContext(adapter);
    if (context->owner == nullptr || input->ConnectorIndex >= Adapter::MaxOutputs)
    {
        return STATUS_INVALID_PARAMETER;
    }
    output->TargetCaps = static_cast<IDDCX_TARGET_CAPS>(
        IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE |
        IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE);
    output->DitheringSupport = {};
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "AdapterQueryTargetInfo: connector=%u HDR10+SDR_WCG RGB10",
        input->ConnectorIndex);
    return STATUS_SUCCESS;
}

} // namespace nv::iddcx
