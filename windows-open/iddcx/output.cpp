#include "output.hpp"
#include "adapter.hpp"
#include "display_modes.hpp"
#include "swapchain_d3d11.hpp"
#include "swapchain_d3d12.hpp"
#include "device.hpp"
#include "driver_parameters.hpp"
#include "hwcursor.h"
#include "trace.h"

#include <cmath>
#include <cstring>
#include <new>

#include "output.tmh"

#ifndef NV_IDDCX_STATIC_GRADIENT
#define NV_IDDCX_STATIC_GRADIENT 1
#endif

namespace nv::iddcx {
namespace {

constexpr NV_KMS_WINDOWS_COLOR_TRANSFORM IdentityColorTransform = {
    {
        { 0x10000, 0, 0, 0 },
        { 0, 0x10000, 0, 0 },
        { 0, 0, 0x10000, 0 },
    },
    {}, {}, {}, NV_FALSE, NV_FALSE,
};

NTSTATUS QueryPixelCachePolicy(NvBool* honorRequested) noexcept
{
    ULONG policy = 0;
    NTSTATUS status = QueryDriverParameterDword(L"SurfaceCachePolicy", &policy);
    if ((!NT_SUCCESS(status) && !DriverParameterAbsent(status)) || policy > 2)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid SurfaceCachePolicy: status=%!STATUS! value=%u", status, policy);
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    }
    ULONG d3d12Mode = 0;
    status = QuerySwapChainMode(&d3d12Mode);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid swapchain settings for surface allocation: status=%!STATUS! mode=%u",
            status, d3d12Mode);
        return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
    }
    *honorRequested = policy == 2 || (policy == 0 && d3d12Mode != 0);
    return STATUS_SUCCESS;
}

NvS32 FloatToCsc(float value) noexcept
{
    if (!std::isfinite(value))
    {
        return 0;
    }
    constexpr float Minimum = -8.0F;
    constexpr float Maximum = 524287.0F / 65536.0F;
    const float clamped = value < Minimum ? Minimum : value > Maximum ? Maximum : value;
    const float scaled = clamped * 65536.0F;
    return static_cast<NvS32>(scaled + (scaled >= 0.0F ? 0.5F : -0.5F));
}

NvU16 FloatToUnorm16(float value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0F)
    {
        return 0;
    }
    if (value >= 1.0F)
    {
        return 0xffff;
    }
    return static_cast<NvU16>(value * 65535.0F + 0.5F);
}

void CopyHdrMetadata(
    const IDDCX_HDR10_METADATA& source,
    NV_KMS_WINDOWS_HDR_METADATA* destination) noexcept
{
    const USHORT* primaries[] = {
        source.RedPrimary, source.GreenPrimary, source.BluePrimary };
    for (NvU32 primary = 0; primary < 3; ++primary)
    {
        destination->displayPrimaries[primary][0] = primaries[primary][0];
        destination->displayPrimaries[primary][1] = primaries[primary][1];
    }
    destination->whitePoint[0] = source.WhitePoint[0];
    destination->whitePoint[1] = source.WhitePoint[1];
    destination->maxMasteringLuminance = source.MaxMasteringLuminance;
    destination->minMasteringLuminance = source.MinMasteringLuminance;
    destination->maxContentLightLevel = source.MaxContentLightLevel;
    destination->maxFrameAverageLightLevel = source.MaxFrameAverageLightLevel;
}

// The optional startup pattern proves scanout before the first OS frame arrives.
void FillGradient(
    void* pixels,
    NvU32 pitch,
    NvU32 width,
    NvU32 height,
    NvU32 outputIndex) noexcept
{
    std::memset(pixels, 0, static_cast<size_t>(pitch) * height);
    auto* bytes = static_cast<NvU8*>(pixels);
    for (NvU32 y = 0; y < height; ++y)
    {
        auto* row = reinterpret_cast<NvU32*>(bytes + pitch * y);
        const NvU32 green = height > 1 ? 255U * y / (height - 1) : 0;
        for (NvU32 x = 0; x < width; ++x)
        {
            const NvU32 red = width > 1 ? 255U * x / (width - 1) : 0;
            const NvU32 blue = 255U - (red + green) / 2U;
            const bool marker = ((x / 64U) ^ (y / 64U) ^ outputIndex) & 1U;
            row[x] = 0xff000000U | ((marker ? red : red / 2U) << 16) |
                ((marker ? green : green / 2U) << 8) | blue;
        }
    }
}

} // namespace

Output::Output(
        Adapter& owner,
        IDDCX_MONITOR monitor,
        const NV_KMS_WINDOWS_DISPLAY_STATE& state) noexcept
        : owner_(&owner),
            displayState_(state),
            monitor_(monitor),
            scanoutSurface_(),
            scanoutPixels_(nullptr),
            scanoutPitch_(0),
            activeModeIndex_(0),
            assignedHead_(0),
            scanoutFormat_(NV_KMS_WINDOWS_SCANOUT_FORMAT_XRGB8),
            colorBpc_(8),
            colorSpace_(NV_KMS_WINDOWS_COLOR_SPACE_SDR),
            defaultHdrMetadata_{},
            hasDefaultHdrMetadata_(false),
            colorTransform_(IdentityColorTransform),
            active_(false),
            stateLock_(),
            swapChain_(nullptr)
{
}

Output::~Output() noexcept
{
    cursor_.reset();
    // The monitor outlives its swapchain context and scanout allocation.
    QuiesceSwapChain();
    DestroyScanout();
    if (owner_ != nullptr)
    {
        owner_->OnOutputDestroyed(this);
        owner_ = nullptr;
    }
}

NTSTATUS Output::Create(
    Adapter& owner,
    const NV_KMS_WINDOWS_DISPLAY_STATE& state,
    Output** output) noexcept
{
    *output = nullptr;
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = display_modes::GetOutputTechnology(state.connectorType);
    info.ConnectorIndex = state.connectorIndex;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = state.edidBytes;
    info.MonitorDescription.pData = const_cast<NvU8*>(state.edid);
    info.MonitorContainerId = {
        0x1500c36c ^ state.displayHandle, 0x73e5, 0x4bd2,
        { 0x93, 0xa7, 0xe6, 0x8c, 0x59, 0xd4, 0xe7, 0x9c } };

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, Output);
    attributes.EvtCleanupCallback = Cleanup;
    IDARG_IN_MONITORCREATE input = {};
    input.ObjectAttributes = &attributes;
    input.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE createOutput = {};
    NTSTATUS status = IddCxMonitorCreate(owner.Handle(), &input, &createOutput);
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

    // IddCx allocated raw context storage; construct the non-heap Output in place.
    Output* instance = ::new (GetOutput(createOutput.MonitorObject)) Output(
        owner,
        createOutput.MonitorObject,
        state);
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "NVKMS monitor: connector=%u physical=%u EDID=%u modes=%u heads=%u layers=%u multiLayer=%u",
        state.connectorType,
        state.connectorIndex,
        state.edidBytes,
        state.modeCount,
        state.headCount,
        state.maxLayersPerHead,
        state.supportsMultiLayerFlip);
    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(instance->monitor_, &arrival);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "IddCxMonitorArrival: %!STATUS! target=%u",
        status,
        arrival.OsTargetId);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(instance->monitor_);
        return status;
    }
    *output = instance;
    return STATUS_SUCCESS;
}

NTSTATUS Output::PrepareScanout(
    const NV_KMS_WINDOWS_MODE& mode,
    NvU32 outputIndex,
    NV_KMS_WINDOWS_SCANOUT_FORMAT format,
    NV_KMS_WINDOWS_SURFACE** surface,
    void** pixels,
    NvU32* pitch) noexcept
{
    NvBool honorPixelCachePolicy = NV_FALSE;
    const NTSTATUS cacheStatus = QueryPixelCachePolicy(&honorPixelCachePolicy);
    if (!NT_SUCCESS(cacheStatus))
    {
        return cacheStatus;
    }
    const NV_STATUS status = NvKmsWindowsCreateScanoutSurface(
        mode.hVisible, mode.vVisible, format, surface, pixels, pitch, honorPixelCachePolicy);
    if (status != NV_OK)
    {
        return status == NV_ERR_NO_MEMORY
            ? STATUS_INSUFFICIENT_RESOURCES
            : STATUS_DEVICE_NOT_READY;
    }
    std::memset(*pixels, 0, static_cast<size_t>(*pitch) * mode.vVisible);
#if NV_IDDCX_STATIC_GRADIENT
    if (format == NV_KMS_WINDOWS_SCANOUT_FORMAT_XRGB8)
    {
        FillGradient(*pixels, *pitch, mode.hVisible, mode.vVisible, outputIndex);
    }
#endif
    return STATUS_SUCCESS;
}

NTSTATUS Output::PrepareBackBuffer() noexcept
{
    auto lock = stateLock_.lock_exclusive();
    if (backSurface_)
    {
        return STATUS_SUCCESS;
    }
    const auto* mode = display_modes::FindDisplayMode(displayState_, activeModeIndex_);
    if (!active_ || !scanoutSurface_ || mode == nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    NV_KMS_WINDOWS_SURFACE* surface = nullptr;
    void* pixels = nullptr;
    NvU32 pitch = 0;
    NvBool honorPixelCachePolicy = NV_FALSE;
    const NTSTATUS cacheStatus = QueryPixelCachePolicy(&honorPixelCachePolicy);
    if (!NT_SUCCESS(cacheStatus))
    {
        return cacheStatus;
    }
    const NV_STATUS status = NvKmsWindowsCreateScanoutSurface(
        mode->hVisible, mode->vVisible, scanoutFormat_, &surface, &pixels, &pitch,
        honorPixelCachePolicy);
    if (status != NV_OK)
    {
        return status == NV_ERR_NO_MEMORY ?
            STATUS_INSUFFICIENT_RESOURCES : STATUS_DEVICE_NOT_READY;
    }
    unique_nvkms_surface allocation(surface);
    if (pitch != scanoutPitch_)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    backSurface_ = std::move(allocation);
    backPixels_ = pixels;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Scanout double buffering: %ux%u pitch=%u format=%u",
        mode->hVisible, mode->vVisible, pitch, scanoutFormat_);
    return STATUS_SUCCESS;
}

void Output::CommitMode(
    bool active,
    NvU32 modeIndex,
    NV_KMS_WINDOWS_SURFACE* newSurface,
    void* newPixels,
    NvU32 newPitch,
    NV_KMS_WINDOWS_SCANOUT_FORMAT format,
    NvU8 colorBpc,
    NV_KMS_WINDOWS_COLOR_SPACE colorSpace) noexcept
{
    if (!active)
    {
        // Imported D3D12 heaps must go away before their WDF backing allocations.
        QuiesceSwapChain();
    }
    // Frame processing holds the shared side through copy and present.
    auto lock = stateLock_.lock_exclusive();
    if (newSurface != nullptr)
    {
        DestroyScanout();
        scanoutSurface_.reset(newSurface);
        scanoutPixels_ = newPixels;
        scanoutPitch_ = newPitch;
    }
    else if (!active)
    {
        DestroyScanout();
    }
    active_ = active;
    activeModeIndex_ = modeIndex;
    scanoutFormat_ = format;
    colorBpc_ = colorBpc;
    colorSpace_ = colorSpace;
}

void Output::SetAssignedHead(NvU32 head) noexcept
{
    auto lock = stateLock_.lock_exclusive();
    assignedHead_ = head;
}

NTSTATUS Output::SetDefaultHdrMetadata(
    const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* arguments) noexcept
{
    if (arguments == nullptr ||
        arguments->Type != IDDCX_HDRMETADATA_TYPE_HDR10 ||
        arguments->Size != sizeof(IDDCX_HDR10_METADATA) ||
        arguments->Data.pHdr10 == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    auto lock = stateLock_.lock_exclusive();
    CopyHdrMetadata(*arguments->Data.pHdr10, &defaultHdrMetadata_);
    hasDefaultHdrMetadata_ = true;
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "Default HDR10 metadata: MaxCLL=%u MaxFALL=%u",
        defaultHdrMetadata_.maxContentLightLevel,
        defaultHdrMetadata_.maxFrameAverageLightLevel);
    return STATUS_SUCCESS;
}

NTSTATUS Output::SetGammaRamp(const IDARG_IN_SET_GAMMARAMP* arguments) noexcept
{
    if (arguments == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NV_KMS_WINDOWS_COLOR_TRANSFORM transform = IdentityColorTransform;
    switch (arguments->Type)
    {
    case IDDCX_GAMMARAMP_TYPE_DEFAULT:
        if (arguments->GammaRampSizeInBytes != 0 ||
            arguments->pGammaRampData != nullptr)
        {
            return STATUS_INVALID_PARAMETER;
        }
        break;
    case IDDCX_GAMMARAMP_TYPE_RGB256x3x16:
        if (arguments->GammaRampSizeInBytes !=
                sizeof(IDDCX_GAMMARAMP_RGB256x3x16) ||
            arguments->pGammaRampData == nullptr)
        {
            return STATUS_INVALID_PARAMETER;
        }
        {
            const auto* ramp = static_cast<const IDDCX_GAMMARAMP_RGB256x3x16*>(
                arguments->pGammaRampData);
            for (NvU32 index = 0; index < NV_KMS_WINDOWS_GAMMA_ENTRIES; ++index)
            {
                const NvU32 source = index * 255U /
                    (NV_KMS_WINDOWS_GAMMA_ENTRIES - 1U);
                transform.gammaRed[index] = ramp->Red[source];
                transform.gammaGreen[index] = ramp->Green[source];
                transform.gammaBlue[index] = ramp->Blue[source];
            }
            transform.gammaEnabled = NV_TRUE;
        }
        break;
    case IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM:
        if (arguments->GammaRampSizeInBytes !=
                sizeof(IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM) ||
            arguments->pGammaRampData == nullptr)
        {
            return STATUS_INVALID_PARAMETER;
        }
        {
            const auto* ramp =
                static_cast<const IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM*>(
                    arguments->pGammaRampData);
            if (ramp->MatrixEnabled)
            {
                for (NvU32 row = 0; row < 3; ++row)
                {
                    for (NvU32 column = 0; column < 4; ++column)
                    {
                        transform.csc[row][column] = FloatToCsc(
                            ramp->ColorMatrix3x4[row][column] *
                            ramp->ScalarMultiplier);
                    }
                }
                transform.cscEnabled = NV_TRUE;
            }
            if (ramp->LutEnabled)
            {
                for (NvU32 index = 0; index < NV_KMS_WINDOWS_GAMMA_ENTRIES; ++index)
                {
                    const NvU32 source = index * 4095U /
                        (NV_KMS_WINDOWS_GAMMA_ENTRIES - 1U);
                    transform.gammaRed[index] =
                        FloatToUnorm16(ramp->LookupTable1D[source].Red);
                    transform.gammaGreen[index] =
                        FloatToUnorm16(ramp->LookupTable1D[source].Green);
                    transform.gammaBlue[index] =
                        FloatToUnorm16(ramp->LookupTable1D[source].Blue);
                }
                transform.gammaEnabled = NV_TRUE;
            }
        }
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }

    auto lock = stateLock_.lock_exclusive();
    colorTransform_ = transform;
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "Gamma ramp: type=%u csc=%u lut=%u",
        arguments->Type,
        colorTransform_.cscEnabled,
        colorTransform_.gammaEnabled);
    return STATUS_SUCCESS;
}

NTSTATUS Output::AssignSwapChain(
    IDDCX_MONITOR monitor,
    const IDARG_IN_SETSWAPCHAIN* arguments) noexcept
{
    if (monitor != monitor_ || arguments == nullptr || !owner_->IsReady())
    {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    QuiesceSwapChain();
    ULONG d3d12Mode = 0;
    const NTSTATUS settingStatus = QuerySwapChainMode(&d3d12Mode);
    if (!NT_SUCCESS(settingStatus))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid swapchain settings: status=%!STATUS! mode=%u; using D3D11 baseline",
            settingStatus, d3d12Mode);
        d3d12Mode = 0;
    }
    if (d3d12Mode == 0)
    {
        d3d12Failed_ = false;
    }
    SwapChainD3D11* swapChain = nullptr;
    SwapChainD3D12* swapChain12 = nullptr;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    if (d3d12Mode != 0 && !d3d12Failed_)
    {
        status = SwapChainD3D12::Create(*this, *arguments, d3d12Mode, &swapChain12);
    }
    if (status == STATUS_NOT_SUPPORTED)
    {
        status = SwapChainD3D11::Create(*this, *arguments, &swapChain);
    }
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // Publish ownership before the worker can report an asynchronous failure.
    {
        auto lock = stateLock_.lock_exclusive();
        swapChain_ = swapChain;
        swapChain12_ = swapChain12;
    }
    const HRESULT startResult = swapChain12 != nullptr
        ? swapChain12->StartProcessing() : swapChain->StartProcessing();
    if (FAILED(startResult))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: worker start failed 0x%08X",
            startResult);
        QuiesceSwapChain();
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    if (displayState_.maxCursorSize != 0)
    {
        if (!cursor_)
        {
            cursor_.reset(new (std::nothrow) HwCursor(*this));
        }
        if (cursor_)
        {
            cursor_->Setup();
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS Output::UnassignSwapChain(IDDCX_MONITOR monitor) noexcept
{
    if (monitor != monitor_)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    QuiesceSwapChain();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "UnassignSwapChain");
    return STATUS_SUCCESS;
}

void Output::OnSwapChainDestroyed(SwapChainD3D11* swapChain) noexcept
{
    auto lock = stateLock_.lock_exclusive();
    if (swapChain_ == swapChain)
    {
        swapChain_ = nullptr;
    }
}

void Output::OnSwapChainDestroyed(SwapChainD3D12* swapChain) noexcept
{
    auto lock = stateLock_.lock_exclusive();
    if (swapChain12_ == swapChain)
    {
        swapChain12_ = nullptr;
    }
    if (swapChain->Failed())
    {
        d3d12Failed_ = true;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 runtime failure: next swapchain uses D3D11 until baseline is reselected or monitor is recreated");
    }
}

void Output::QuiesceSwapChain() noexcept
{
    SwapChainD3D11* swapChain = nullptr;
    SwapChainD3D12* swapChain12 = nullptr;
    {
        auto lock = stateLock_.lock_exclusive();
        swapChain = swapChain_;
        swapChain_ = nullptr;
        swapChain12 = swapChain12_;
        swapChain12_ = nullptr;
    }
    if (swapChain != nullptr)
    {
        swapChain->StopAndDelete();
    }
    if (swapChain12 != nullptr)
    {
        swapChain12->StopAndDelete();
    }
}

void Output::PauseCursor() noexcept
{
    if (cursor_)
    {
        cursor_->Pause();
    }
}

void Output::ResumeCursor() noexcept
{
    if (cursor_)
    {
        cursor_->Resume();
    }
}

void Output::DestroyScanout() noexcept
{
    backSurface_.reset();
    backPixels_ = nullptr;
    frontBufferIndex_.store(0);
    scanoutSurface_.reset();
    scanoutPixels_ = nullptr;
    scanoutPitch_ = 0;
}

_Use_decl_annotations_
void Output::Cleanup(WDFOBJECT monitorObject)
{
    GetOutput(monitorObject)->~Output();
}

_Use_decl_annotations_
NTSTATUS GetDefaultDescriptionModes(IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* input,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output)
{
    Output* context = GetOutput(monitor);
    if (context == nullptr || input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return display_modes::FillMonitorModeBuffer(
        context->DisplayState(),
        input->DefaultMonitorModeBufferInputCount,
        input->pDefaultMonitorModes,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
        &output->DefaultMonitorModeBufferOutputCount,
        &output->PreferredMonitorModeIdx);
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    Output* context = GetOutput(monitor);
    if (context == nullptr || input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    const auto& state = context->DisplayState();
    output->TargetModeBufferOutputCount = state.modeCount;
    if (input->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (input->TargetModeBufferInputCount < state.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < state.modeCount; ++index)
    {
        input->pTargetModes[index] = display_modes::CreateTargetMode(state.modes[index]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS QueryTargetModes2(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES2* input,
    IDARG_OUT_QUERYTARGETMODES* output)
{
    Output* context = GetOutput(monitor);
    if (context == nullptr || input == nullptr || output == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    const auto& state = context->DisplayState();
    output->TargetModeBufferOutputCount = state.modeCount;
    if (input->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (input->pTargetModes == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (input->TargetModeBufferInputCount < state.modeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT index = 0; index < state.modeCount; ++index)
    {
        input->pTargetModes[index] = display_modes::CreateTargetMode2(state.modes[index]);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* input)
{
    Output* context = GetOutput(monitor);
    return context == nullptr
        ? STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN
        : context->AssignSwapChain(monitor, input);
}

_Use_decl_annotations_
NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor)
{
    Output* context = GetOutput(monitor);
    return context == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->UnassignSwapChain(monitor);
}

_Use_decl_annotations_
NTSTATUS SetDefaultHdrMetadata(
    IDDCX_MONITOR monitor,
    const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* input)
{
    Output* context = GetOutput(monitor);
    return context == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->SetDefaultHdrMetadata(input);
}

_Use_decl_annotations_
NTSTATUS SetGammaRamp(IDDCX_MONITOR monitor, const IDARG_IN_SET_GAMMARAMP* input)
{
    Output* context = GetOutput(monitor);
    return context == nullptr
        ? STATUS_INVALID_DEVICE_STATE
        : context->SetGammaRamp(input);
}

} // namespace nv::iddcx
