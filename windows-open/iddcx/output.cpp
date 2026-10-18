#include "output.hpp"
#include "mpo_scene.hpp"
#include "adapter.hpp"
#include "display_modes.hpp"
#include "swapchain_d3d11.hpp"
#include "swapchain_d3d12.hpp"
#include "device.hpp"
#include "driver_parameters.hpp"
#include "hwcursor.h"
#include "trace.h"
#include "monitor_i2c.hpp"

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
    Parameters().UnregisterOutput(*this);
    StopMonitorControl();
    cursor_.reset();
    // The monitor outlives its swapchain context and scanout allocation.
    QuiesceSwapChain();
    DestroyScanout();
    Parameters().Report(displayState_.connectorIndex, false, appliedGeneration_.load(),
        0, 0, 0, false, false, false);
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
    Parameters().RegisterOutput(*instance);
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
    const NvBool honorPixelCachePolicy = Parameters().Snapshot().HonorCache();
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
    const auto* mode = display_modes::FindDisplayMode(displayState_, activeModeIndex_);
    if (!active_ || !scanoutSurface_ || mode == nullptr)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    const NvBool honorPixelCachePolicy = config_.HonorCache();
    for (NvU32 index = 1; index < presentationPolicy_.buffers; ++index)
    {
        if (Surface(index) != nullptr)
            continue;
        NV_KMS_WINDOWS_SURFACE* surface = nullptr;
        void* pixels = nullptr;
        NvU32 pitch = 0;
        const NV_STATUS status = NvKmsWindowsCreateScanoutSurface(
            mode->hVisible, mode->vVisible, scanoutFormat_, &surface, &pixels, &pitch,
            honorPixelCachePolicy);
        if (status != NV_OK)
            return status == NV_ERR_NO_MEMORY ?
                STATUS_INSUFFICIENT_RESOURCES : STATUS_DEVICE_NOT_READY;
        unique_nvkms_surface allocation(surface);
        if (pitch != scanoutPitch_)
            return STATUS_INVALID_DEVICE_STATE;
        if (index == 1)
        {
            backSurface_ = std::move(allocation);
            backPixels_ = pixels;
        }
        else
        {
            extraSurfaces_[index - 2] = std::move(allocation);
            extraPixels_[index - 2] = pixels;
        }
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Scanout buffering: count=%u %ux%u pitch=%u format=%u",
        presentationPolicy_.buffers, mode->hVisible, mode->vVisible, scanoutPitch_, scanoutFormat_);
    return STATUS_SUCCESS;
}

NTSTATUS Output::ConfigurePresentationPolicy() noexcept
{
    config_ = Parameters().Snapshot();
    if (config_.connector != displayState_.connectorIndex) config_.demo = 0;
    requestedGeneration_.store(config_.generation);
    const PresentationPolicy policy = config_.presentation;
    presentationPolicy_ = policy;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Presentation policy: vrr=%u flip=%u pipeline=%u buffers=%u",
        policy.vrr, policy.flip, policy.pipeline, policy.buffers);
    return STATUS_SUCCESS;
}

NTSTATUS Output::StageConfiguration(const SwapChainConfig& candidate) noexcept
{
    if (RequiresScanoutReset()) return STATUS_INVALID_DEVICE_STATE;
    const NTSTATUS vrr = ValidateVrr(candidate);
    if (!NT_SUCCESS(vrr)) return vrr;
    pendingScene_.reset();
    if (candidate.demo != 0 && candidate.connector == displayState_.connectorIndex)
    {
        const HRESULT result = MpoScene::Create(*this, candidate, &pendingScene_);
        if (FAILED(result))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "MPO scene staging failed: connector=%u mode=%u result=0x%08X",
                candidate.connector, candidate.demo, result);
            return STATUS_NOT_SUPPORTED;
        }
    }
    if (candidate.HonorCache() == config_.HonorCache() &&
        candidate.presentation.buffers == config_.presentation.buffers)
        return STATUS_SUCCESS;
    if (retiredFrontPending_.load()) return STATUS_DEVICE_BUSY;
    const auto* mode = display_modes::FindDisplayMode(displayState_, activeModeIndex_);
    if (!mode || !active_) return STATUS_INVALID_DEVICE_STATE;
    std::unique_ptr<ScanoutPool> pool(new (std::nothrow) ScanoutPool);
    if (!pool) return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG index = 0; index < candidate.presentation.buffers; ++index)
    {
        NV_KMS_WINDOWS_SURFACE* surface = nullptr;
        NvU32 pitch = 0;
        const NV_STATUS status = NvKmsWindowsCreateScanoutSurface(mode->hVisible,
            mode->vVisible, scanoutFormat_, &surface, &pool->pixels[index], &pitch,
            candidate.HonorCache());
        if (status != NV_OK)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Replacement scanout allocation failed: connector=%u slot=%u cache=%u status=0x%08X",
                displayState_.connectorIndex, index, candidate.HonorCache(), status);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pool->surfaces[index].reset(surface);
        if (index != 0 && pitch != pool->pitch) return STATUS_INVALID_DEVICE_STATE;
        pool->pitch = pitch;
    }
    pendingPool_ = std::move(pool);
    return STATUS_SUCCESS;
}

HRESULT Output::ValidateConfiguration(const SwapChainConfig& config) noexcept
{
    auto guard = stateLock_.lock_shared();
    if (!active_)
        return config.demo != 0 && config.connector == displayState_.connectorIndex ?
            E_UNEXPECTED : S_OK;
    if (RequiresScanoutReset()) return E_UNEXPECTED;
    const NTSTATUS vrr = ValidateVrr(config);
    if (!NT_SUCCESS(vrr)) return HRESULT_FROM_NT(vrr);
    if (config.demo == 0 || config.connector != displayState_.connectorIndex) return S_OK;
    std::unique_ptr<MpoScene> candidate;
    return MpoScene::Create(*this, config, &candidate);
}

HRESULT Output::CheckConfiguration() noexcept
{
    auto guard = stateLock_.lock_shared();
    const auto candidate = Parameters().Snapshot();
    if (candidate.generation == checkedGeneration_) return S_OK;
    SwapChainConfig local = candidate;
    if (local.demo != 0 && local.connector != displayState_.connectorIndex)
    {
        local.demo = 0;
        if (config_.demo == 0)
        {
            local.demoPlanes = config_.demoPlanes;
            local.connector = config_.connector;
            local.cadence = config_.cadence;
            local.speed = config_.speed;
        }
    }
    const bool retryNative = d3d12Failed_ && local.Mode() != 0 &&
        local.revision != config_.revision;
    if (config_.Equivalent(local) && !retryNative)
    {
        checkedGeneration_ = candidate.generation;
        requestedGeneration_.store(candidate.generation);
        if (presentationVisible_.load() && appliedGeneration_.load() >= config_.generation)
            PresentationApplied();
        return S_OK;
    }
    const NTSTATUS staged = StageConfiguration(local);
    if (staged == STATUS_DEVICE_BUSY) return S_OK;
    checkedGeneration_ = candidate.generation;
    if (!NT_SUCCESS(staged))
    {
        Parameters().Reject("Cannot stage replacement scanout resources; previous configuration retained");
        return S_OK;
    }
    pendingConfig_ = local;
    havePendingConfig_ = true;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Swapchain reconfiguration requested: connector=%u generation=%u",
        displayState_.connectorIndex, candidate.generation);
    return DXGI_ERROR_ACCESS_LOST;
}

NTSTATUS Output::ValidateVrr(const SwapChainConfig& config) noexcept
{
    const bool requested = config.presentation.vrr != 0;
    if (requested == vrrEnabled_) return STATUS_SUCCESS;
    const NV_STATUS status = NvKmsWindowsValidateVrrPreference(
        assignedHead_, displayState_.displayHandle, requested);
    if (status == NV_OK) return STATUS_SUCCESS;
    if (status == NV_ERR_NOT_SUPPORTED && requested && !vrrEnabled_)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "VRR preference unavailable: connector=%u requested=1 effective=0",
            displayState_.connectorIndex);
        return STATUS_SUCCESS;
    }
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
        "VRR preflight failed: connector=%u requested=%u effective=%u status=0x%08X",
        displayState_.connectorIndex, requested, vrrEnabled_, status);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS Output::DrainPresentation() noexcept
{
    const NvU32 count = scene_ ? scene_->PlaneCount() + 1 : 1;
    const auto deadline = GetTickCount64() + 2000;
    for (;;)
    {
        bool busy = false;
        for (NvU32 index = 0; index < count; ++index)
        {
            NvBool pending = NV_FALSE;
            const NV_STATUS status = NvKmsWindowsGetLayerFlipPending(assignedHead_, index, &pending);
            if (status != NV_OK)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Presentation drain query failed: connector=%u layer=%u status=0x%08X",
                    displayState_.connectorIndex, index, status);
                RequireScanoutReset();
                return STATUS_INVALID_DEVICE_STATE;
            }
            busy = busy || pending != NV_FALSE;
        }
        if (!busy) return STATUS_SUCCESS;
        if (GetTickCount64() > deadline)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Presentation drain timed out: connector=%u layers=%u",
                displayState_.connectorIndex, count);
            RequireScanoutReset();
            return STATUS_IO_TIMEOUT;
        }
        Sleep(1);
    }
}

NTSTATUS Output::ApplyConfiguration() noexcept
{
    if (RequiresScanoutReset()) return STATUS_INVALID_DEVICE_STATE;
    if (!havePendingConfig_) return STATUS_SUCCESS;
    auto lock = stateLock_.lock_exclusive();
    const bool requestedVrr = pendingConfig_.presentation.vrr != 0;
    if (requestedVrr != vrrEnabled_)
    {
        const NV_STATUS validation = NvKmsWindowsValidateVrrPreference(
            assignedHead_, displayState_.displayHandle, requestedVrr);
        NV_STATUS status = validation;
        if (validation == NV_OK)
        {
            const NTSTATUS drained = DrainPresentation();
            if (!NT_SUCCESS(drained)) return drained;
            NvBool enabled = NV_FALSE;
            status = NvKmsWindowsSetVrrPreference(
                assignedHead_, displayState_.displayHandle, requestedVrr, &enabled);
            if (status == NV_OK) vrrEnabled_ = enabled != NV_FALSE;
            const NTSTATUS retired = DrainPresentation();
            if (!NT_SUCCESS(retired)) return retired;
        }
        if (status == NV_ERR_INVALID_STATE)
        {
            RequireScanoutReset();
            Parameters().Reject("VRR rollback failed; scanout leases retained for recovery");
            return STATUS_INVALID_DEVICE_STATE;
        }
        if (status != NV_OK && !(status == NV_ERR_NOT_SUPPORTED && requestedVrr && !vrrEnabled_))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "VRR application failed: connector=%u status=0x%08X; previous configuration retained",
                displayState_.connectorIndex, status);
            Parameters().Reject("Cannot apply VRR policy; previous configuration retained");
            pendingScene_.reset();
            pendingPool_.reset();
            havePendingConfig_ = false;
            return STATUS_SUCCESS;
        }
        if (status != NV_OK)
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "VRR preference remains unavailable: connector=%u effective=0",
                displayState_.connectorIndex);
    }
    if (pendingPool_)
    {
        const ULONG front = frontBufferIndex_.load();
        retiredFront_ = front == 0 ? std::move(scanoutSurface_) : front == 1 ?
            std::move(backSurface_) : std::move(extraSurfaces_[front - 2]);
        retiredFrontPending_.store(true);
        scanoutSurface_ = std::move(pendingPool_->surfaces[0]);
        backSurface_ = std::move(pendingPool_->surfaces[1]);
        for (UINT index = 0; index < 2; ++index)
        {
            extraSurfaces_[index] = std::move(pendingPool_->surfaces[index + 2]);
            extraPixels_[index] = pendingPool_->pixels[index + 2];
        }
        scanoutPixels_ = pendingPool_->pixels[0];
        backPixels_ = pendingPool_->pixels[1];
        scanoutPitch_ = pendingPool_->pitch;
        frontBufferIndex_.store(0);
        submittedBufferIndex_.store(0);
        copyBufferIndex_.store(1);
        pendingPool_.reset();
    }
    config_ = pendingConfig_;
    requestedGeneration_.store(config_.generation);
    presentationPolicy_ = config_.presentation;
    scene_ = std::move(pendingScene_);
    havePendingConfig_ = false;
    d3d12Failed_ = false;
    return STATUS_SUCCESS;
}

void Output::RetirePreviousPool() noexcept
{
    retiredFront_.reset();
    retiredFrontPending_.store(false);
}

void Output::PresentationApplied() noexcept
{
    presentationVisible_.store(true);
    const ULONG generation = requestedGeneration_.load();
    if (appliedGeneration_.exchange(generation) != generation)
        Parameters().Applied(displayState_.connectorIndex, generation);
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
    if (!active)
        Parameters().Report(displayState_.connectorIndex, false, appliedGeneration_.load(),
            0, 0, displayState_.maxLayersPerHead, false, false, false);
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
    presentationVisible_.store(false);
    const NTSTATUS appliedConfig = ApplyConfiguration();
    if (!NT_SUCCESS(appliedConfig))
    {
        Parameters().Reject("Cannot retire presentation safely; scanout recovery required");
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    if (config_.demo != 0 && config_.connector == displayState_.connectorIndex && !scene_)
    {
        const HRESULT created = MpoScene::Create(*this, config_, &scene_);
        if (FAILED(created))
        {
            Parameters().Reject("Requested MPO mode unsupported on this active output");
            return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
        }
    }
    const ULONG d3d12Mode = config_.Mode();
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
    NV_KMS_WINDOWS_LAYER_CAPS caps = {};
    const NV_STATUS capsStatus = NvKmsWindowsGetLayerCaps(assignedHead_, 1, &caps);
    if (capsStatus != NV_OK)
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "MPO capability query unavailable: connector=%u status=0x%08X",
            displayState_.connectorIndex, capsStatus);
    const auto* mode = display_modes::FindDisplayMode(displayState_, activeModeIndex_);
    const ULONG applied = appliedGeneration_.load();
    Parameters().Report(displayState_.connectorIndex, true, applied,
        swapChain12 != nullptr ? 12 : 11,
        config_.connector == displayState_.connectorIndex ? config_.demo : 0,
        displayState_.maxLayersPerHead, caps.rotationMask == 15 && caps.reflectionSupported,
        caps.inputLutSupported != 0, caps.gainOverrideSupported != 0, caps.pixelAlphaSupported != 0,
        mode ? mode->refreshRateMillihertz : 0, caps.cscSupported != 0, vrrEnabled_);
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

bool Output::HardwareRemoved() const noexcept
{
    return IsHardwareRemoved(owner_->Device());
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
    pendingScene_.reset();
    scene_.reset();
    pendingPool_.reset();
    retiredFront_.reset();
    retiredFrontPending_.store(false);
    havePendingConfig_ = false;
    for (auto& surface : extraSurfaces_)
        surface.reset();
    for (auto& pixels : extraPixels_)
        pixels = nullptr;
    backSurface_.reset();
    backPixels_ = nullptr;
    frontBufferIndex_.store(0);
    copyBufferIndex_.store(1);
    submittedBufferIndex_.store(0);
    scanoutRequiresReset_.store(false);
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

NTSTATUS Output::MonitorI2c(
    NvU32 transmitAddress, NvU32 transmitSize, const NvU8* transmit,
    NvU32 receiveAddress, NvU32 receiveSize, NvU8* receive,
    NvU32 receiveFlags) noexcept
{
    const WDFDEVICE device = owner_->Device();
    const ULONGLONG start = GetTickCount64();
    NTSTATUS status = WdfDeviceStopIdle(device, FALSE);
    NV_STATUS nvStatus = NV_ERR_NOT_READY;
    if (NT_SUCCESS(status))
    {
        DeviceContext* const context = GetDeviceContext(device);
        bool locked = false;
        while (!(locked = TryAcquireSRWLockShared(&context->lifecycleLock)))
        {
            if (GetTickCount64() - start >= 1000)
            {
                nvStatus = NV_ERR_BUSY_RETRY;
                break;
            }
            Sleep(1);
        }
        if (locked)
        {
            if (InterlockedCompareExchange(&context->d0Active, 0, 0) != 0 &&
                context->nvKmsInitialized && monitorControlAvailable_.load())
            {
                nvStatus = NvKmsWindowsMonitorI2c(displayState_.displayHandle,
                    monitor_i2c::WindowsAddress(transmitAddress, false), transmitSize, transmit,
                    monitor_i2c::WindowsAddress(receiveAddress, true), receiveSize, receive, receiveFlags);
            }
            ReleaseSRWLockShared(&context->lifecycleLock);
        }
        WdfDeviceResumeIdle(device);
        switch (nvStatus)
        {
        case NV_OK: status = STATUS_SUCCESS; break;
        case NV_ERR_INVALID_ARGUMENT: status = STATUS_INVALID_PARAMETER; break;
        case NV_ERR_NOT_SUPPORTED: status = STATUS_NOT_SUPPORTED; break;
        case NV_ERR_BUFFER_TOO_SMALL: status = STATUS_BUFFER_TOO_SMALL; break;
        case NV_ERR_OBJECT_NOT_FOUND: status = STATUS_GRAPHICS_I2C_DEVICE_DOES_NOT_EXIST; break;
        case NV_ERR_TIMEOUT: status = STATUS_IO_TIMEOUT; break;
        case NV_ERR_BUSY_RETRY: status = STATUS_DEVICE_BUSY; break;
        case NV_ERR_INVALID_STATE:
        case NV_ERR_NOT_READY: status = STATUS_DEVICE_NOT_READY; break;
        default: status = receiveSize != 0 ?
            STATUS_GRAPHICS_I2C_ERROR_RECEIVING_DATA :
            STATUS_GRAPHICS_I2C_ERROR_TRANSMITTING_DATA; break;
        }
    }
    TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "Monitor I2C: display=0x%08X txAddr=0x%02X tx=%u rxAddr=0x%02X rx=%u flags=%u nv=0x%08X status=%!STATUS! elapsedMs=%llu",
        displayState_.displayHandle, transmitAddress, transmitSize,
        receiveAddress, receiveSize, receiveFlags, nvStatus, status, GetTickCount64() - start);
    return status;
}

_Use_decl_annotations_
NTSTATUS MonitorI2cTransmit(IDDCX_MONITOR monitor, const IDARG_IN_I2C_TRANSMIT* input)
{
    if (input == nullptr || input->DataSizeInBytes == 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Invalid monitor I2C transmit request");
        return STATUS_INVALID_PARAMETER;
    }
    return GetOutput(monitor)->MonitorI2c(input->SevenBitI2CAddress,
        input->DataSizeInBytes, static_cast<const NvU8*>(input->pData),
        0, 0, nullptr, 0);
}

_Use_decl_annotations_
NTSTATUS MonitorI2cReceive(IDDCX_MONITOR monitor, const IDARG_IN_I2C_RECEIVE* input)
{
    if (input == nullptr || input->DataSizeInBytes == 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Invalid monitor I2C receive request");
        return STATUS_INVALID_PARAMETER;
    }
    return GetOutput(monitor)->MonitorI2c(0, 0, nullptr,
        input->SevenBitI2CAddress, input->DataSizeInBytes,
        static_cast<NvU8*>(input->pData), input->Flags);
}

_Use_decl_annotations_
NTSTATUS MonitorI2cTransmitAndReceive(
    IDDCX_MONITOR monitor, const IDARG_IN_I2C_TRANSMIT_AND_RECEIVE* input)
{
    if (input == nullptr || input->TransmitDataSizeInBytes == 0 ||
        input->ReceiveDataSizeInBytes == 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Invalid combined monitor I2C request");
        return STATUS_INVALID_PARAMETER;
    }
    return GetOutput(monitor)->MonitorI2c(input->TransmitSevenBitI2CAddress,
        input->TransmitDataSizeInBytes, static_cast<const NvU8*>(input->pTransmitData),
        input->ReceiveSevenBitI2CAddress, input->ReceiveDataSizeInBytes,
        static_cast<NvU8*>(input->pReceiveData), input->ReceiveFlags);
}

} // namespace nv::iddcx
