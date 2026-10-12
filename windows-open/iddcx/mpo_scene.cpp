#include "mpo_scene.hpp"
#include "display_modes.hpp"
#include "trace.h"
#include <new>

#include "mpo_scene.tmh"

namespace nv::iddcx {

HRESULT MpoScene::Create(Output& output, const SwapChainConfig& config,
    std::unique_ptr<MpoScene>* scene) noexcept
{
    std::unique_ptr<MpoScene> candidate(new (std::nothrow) MpoScene);
    if (!candidate) return E_OUTOFMEMORY;
    candidate->output_ = &output;
    candidate->config_ = config;
    const HRESULT result = candidate->Initialize();
    if (FAILED(result)) return result;
    *scene = std::move(candidate);
    return S_OK;
}

HRESULT MpoScene::Initialize() noexcept
{
    LARGE_INTEGER frequency = {};
    if (!cadence_.Configure(config_.cadence) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0 ||
        static_cast<ULONGLONG>(frequency.QuadPart) >
            (std::numeric_limits<ULONGLONG>::max)() / mpo::FrameCadence::TicksPerSecond)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "MPO cadence clock initialization failed");
        return E_INVALIDARG;
    }
    frequency_ = static_cast<ULONGLONG>(frequency.QuadPart);
    const auto* mode = display_modes::FindDisplayMode(
        output_->DisplayState(), output_->ActiveModeIndex());
    if (!mode || !output_->Active()) return E_UNEXPECTED;
    width_ = mode->hVisible;
    height_ = mode->vVisible;
    if (width_ < 640 || height_ < 480 ||
        config_.cadence * 1000 > mode->refreshRateMillihertz + 100 ||
        config_.demoPlanes + 1 > output_->DisplayState().maxLayersPerHead)
        return DXGI_ERROR_UNSUPPORTED;
    NV_KMS_WINDOWS_LAYER layers[8] = {};
    layers[0].surface = output_->ScanoutSurface();
    layers[0].sourceWidth = layers[0].destinationWidth = width_;
    layers[0].sourceHeight = layers[0].destinationHeight = height_;
    layers[0].opacity = 255;
    layers[0].colorSpace = output_->ColorSpace();
    layers[0].skipUpload = NV_TRUE;
    layers[0].colorTransform = &output_->ColorTransform();
    layers[0].hdrMetadata = output_->DefaultHdrMetadata();
    layers[0].hdrMetadataEnabled = output_->HasDefaultHdrMetadata();
    layers[0].presentationPolicySpecified = NV_TRUE;
    layers[0].immediateNonTearing = config_.presentation.flip != 0;
    layers[0].vrrEnabled = config_.presentation.vrr != 0 && output_->VrrEnabled();
    for (unsigned index = 0; index < config_.demoPlanes; ++index)
    {
        NV_KMS_WINDOWS_LAYER_CAPS caps = {};
        if (NvKmsWindowsGetLayerCaps(output_->AssignedHead(), index + 1, &caps) != NV_OK)
            return E_FAIL;
        if (index == 0) caps_ = caps;
        const bool alpha = config_.demo == 1;
        const bool geometry = config_.demo == 2;
        const bool lut = config_.demo == 5;
        if (!caps.pixelAlphaSupported) features_ &= ~(1U << 1);
        if (caps.transformMask == 0) features_ &= ~(1U << 2);
        if (!caps.cscSupported) features_ &= ~(1U << 3);
        if (!caps.gainOverrideSupported) features_ &= ~(1U << 4);
        if (!caps.inputLutSupported) features_ &= ~(1U << 5);
        if ((alpha && !caps.pixelAlphaSupported) ||
            (geometry && caps.transformMask == 0) ||
            (lut && !caps.inputLutSupported))
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "MPO feature unsupported: connector=%u plane=%u mode=%u rotations=%u reflection=%u pixelAlpha=%u inputLut=%u",
                config_.connector, index + 1, config_.demo, caps.rotationMask,
                caps.reflectionSupported, caps.pixelAlphaSupported, caps.inputLutSupported);
            return DXGI_ERROR_UNSUPPORTED;
        }
        auto& plane = planes_[index];
        plane.transformMask = caps.transformMask;
        if ((geometry || config_.demo == 6) && caps.transformMask != 0xff)
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                "MPO geometry uses supported transforms only: connector=%u plane=%u mask=0x%X; unsupported angles/reflections omitted",
                config_.connector, index + 1, caps.transformMask);
        for (auto& buffer : plane.buffers)
        {
            NV_KMS_WINDOWS_SURFACE* surface = nullptr;
            const NV_STATUS allocated = NvKmsWindowsCreateScanoutSurface(mpo::Width,
                mpo::Height, NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8,
                &surface, &buffer.pixels, &buffer.pitch, NV_TRUE);
            if (allocated != NV_OK) return E_OUTOFMEMORY;
            buffer.surface.reset(surface);
        }
        if (lut || (config_.demo == 6 && caps.inputLutSupported))
        {
            for (unsigned kind = 0; kind < mpo::LutCount; ++kind)
            {
                NV_KMS_WINDOWS_COLOR_TRANSFORM transform = {};
                transform.gammaEnabled = NV_TRUE;
                for (unsigned entry = 0; entry < NV_KMS_WINDOWS_GAMMA_ENTRIES; ++entry)
                {
                    const float value = static_cast<float>(entry) /
                        (NV_KMS_WINDOWS_GAMMA_ENTRIES - 1);
                    const auto curved = static_cast<NvU16>(mpo::Curve(kind, value) * 65535 + 0.5F);
                    transform.gammaRed[entry] = transform.gammaGreen[entry] =
                        transform.gammaBlue[entry] = curved;
                }
                if (config_.demo == 6)
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                        "MPO showcase supported chapter mask: connector=%u features=0x%X; omitted chapters are unsupported",
                        config_.connector, features_);
                NV_KMS_WINDOWS_SURFACE* surface = nullptr;
                if (NvKmsWindowsCreateInputLut(&transform, &surface) != NV_OK) return E_FAIL;
                plane.luts[kind].reset(surface);
            }
        }
    }
    if ((config_.demo == 2 || config_.demo == 6) && (features_ & (1U << 2)))
    {
        for (unsigned index = 0; index < config_.demoPlanes; ++index)
        {
            unsigned supported = 0;
            for (unsigned scale = 0; scale < 3; ++scale)
            {
                planes_[index].scaleMask = 1U << scale;
                bool valid = true;
                for (unsigned step = 0; step < 24 && valid; ++step)
                {
                    const double seconds = static_cast<double>(step) * 0.5 * 100 / config_.speed;
                    for (unsigned plane = 0; plane < config_.demoPlanes; ++plane)
                        Describe(plane, 0, seconds, &layers[plane + 1], 2);
                    const NV_STATUS status = NvKmsWindowsValidateLayers(output_->AssignedHead(),
                        output_->DisplayState().displayHandle, layers,
                        config_.demoPlanes + 1);
                    if (status != NV_OK)
                    {
                        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                            "MPO geometry scale preflight rejected: connector=%u plane=%u scale=%u state=%u status=0x%08X",
                            config_.connector, index + 1, scale, step, status);
                        if (status != NV_ERR_NOT_SUPPORTED && status != NV_ERR_INVALID_STATE)
                            return E_FAIL;
                        valid = false;
                    }
                }
                if (valid) supported |= 1U << scale;
                planes_[index].scaleMask = 2;
            }
            if (supported == 0)
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                    "MPO geometry has no validated scale state: connector=%u plane=%u",
                    config_.connector, index + 1);
                return DXGI_ERROR_UNSUPPORTED;
            }
            planes_[index].scaleMask = supported;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                "MPO geometry validated scale mask: connector=%u plane=%u mask=0x%X (0.5x/1x/1.5x); rejected scales omitted",
                config_.connector, index + 1, supported);
        }
    }
    // Validate every finite effect state before disturbing the running swapchain.
    for (unsigned step = 0; step < 240; ++step)
    {
        const double seconds = static_cast<double>(step) * 0.5 * 100 / config_.speed;
        for (unsigned index = 0; index < config_.demoPlanes; ++index)
            Describe(index, 0, seconds, &layers[index + 1]);
        const NV_STATUS validated = NvKmsWindowsValidateLayers(output_->AssignedHead(),
            output_->DisplayState().displayHandle, layers, config_.demoPlanes + 1);
        if (validated != NV_OK)
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "MPO preflight rejected: connector=%u mode=%u state=%u status=0x%08X",
                config_.connector, config_.demo, step, validated);
            return DXGI_ERROR_UNSUPPORTED;
        }
    }
    started_ = Clock100ns();
    return S_OK;
}

void MpoScene::Describe(unsigned index, unsigned buffer, double seconds,
    NV_KMS_WINDOWS_LAYER* layer, unsigned animationMode) noexcept
{
    auto& plane = planes_[index];
    const auto animation = mpo::Animate(animationMode != 0 ? animationMode : config_.demo,
        seconds, config_.speed, index, features_, plane.transformMask, plane.scaleMask);
    const auto matrix = mpo::ColorMatrix(animation);
    plane.transform = {};
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column)
            plane.transform.csc[row][column] = static_cast<NvS32>(matrix[row][column] * 65536);
    plane.transform.cscEnabled = animation.csc || animation.effect == 4 || animation.gain != 1;
    *layer = {};
    layer->surface = plane.buffers[buffer].surface.get();
    layer->sourceWidth = mpo::Width;
    layer->sourceHeight = mpo::Height;
    if (animation.effect == 2 && animation.variant == 4)
    {
        // KAPI window layers expose source extents, but not fetch offsets.
        layer->sourceWidth -= 64;
        layer->sourceHeight -= 48;
    }
    layer->destinationWidth = static_cast<NvU32>(layer->sourceWidth * animation.scale);
    layer->destinationHeight = static_cast<NvU32>(layer->sourceHeight * animation.scale);
    if (animation.rotation == 90 || animation.rotation == 270)
        std::swap(layer->destinationWidth, layer->destinationHeight);
    layer->destinationX = static_cast<NvU32>((width_ - layer->destinationWidth) * animation.orbitX);
    layer->destinationY = static_cast<NvU32>((height_ - layer->destinationHeight) * animation.orbitY);
    layer->rotationDegrees = animation.rotation;
    layer->reflectX = animation.reflection;
    layer->pixelAlpha = animation.pixelAlpha;
    layer->opacity = animation.opacity;
    layer->colorSpace = NV_KMS_WINDOWS_COLOR_SPACE_SDR;
    layer->colorTransform = &plane.transform;
    layer->inputLut = (config_.demo != 6 || (features_ & (1U << 5))) ?
        plane.luts[animation.lut].get() : nullptr;
    layer->skipUpload = NV_TRUE;
    layer->presentationPolicySpecified = NV_TRUE;
    layer->immediateNonTearing = config_.presentation.flip != 0;
    layer->vrrEnabled = config_.presentation.vrr != 0 && output_->VrrEnabled();
}

HRESULT MpoScene::Retire(bool* pending) noexcept
{
    *pending = false;
    for (unsigned index = 0; index < config_.demoPlanes; ++index)
    {
        auto& plane = planes_[index];
        // A never-submitted layer has no valid notifier timestamp to retire.
        if (plane.slots.Front() == FrameInvalid && plane.slots.Queued() == FrameInvalid)
            continue;
        NvBool busy = NV_FALSE;
        if (NvKmsWindowsGetLayerFlipPending(output_->AssignedHead(), index + 1, &busy) != NV_OK)
            return E_FAIL;
        *pending = *pending || busy != NV_FALSE;
        if (!busy && plane.slots.Queued() != FrameInvalid)
        {
            NvU64 timestamp = 0;
            if (NvKmsWindowsGetLayerFlipTimestamp(
                output_->AssignedHead(), index + 1, &timestamp) != NV_OK) return E_FAIL;
            plane.timestamp = timestamp;
            if (FAILED(plane.slots.Latch())) return E_UNEXPECTED;
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DRIVER,
                "MPO layer retired: connector=%u layer=%u slot=%u timestamp=%llu",
                config_.connector, index + 1, plane.slots.Front(), timestamp);
        }
    }
    return S_OK;
}

bool MpoScene::Due() const noexcept
{
    return cadence_.Due(Clock100ns());
}

ULONGLONG MpoScene::Clock100ns() const noexcept
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    const auto ticks = static_cast<ULONGLONG>(counter.QuadPart);
    return ticks / frequency_ * mpo::FrameCadence::TicksPerSecond +
        ticks % frequency_ * mpo::FrameCadence::TicksPerSecond / frequency_;
}

HRESULT MpoScene::Prepare(const NV_KMS_WINDOWS_LAYER& primary,
    NV_KMS_WINDOWS_LAYER* layers, NvU32* count) noexcept
{
    layers[0] = primary;
    preparedAnimation_ = Due();
    const double seconds = static_cast<double>(Clock100ns() - started_) /
        mpo::FrameCadence::TicksPerSecond;
    for (unsigned index = 0; index < config_.demoPlanes; ++index)
    {
        auto& plane = planes_[index];
        if (!preparedAnimation_ && plane.slots.Front() != FrameInvalid)
        {
            layers[index + 1] = plane.submittedLayer;
            layers[index + 1].reuseSurface = NV_TRUE;
            if (plane.slots.Acquire(true) == FrameInvalid) return E_UNEXPECTED;
            continue;
        }
        const UINT available = plane.slots.Acquire();
        if (available == FrameInvalid) return HRESULT_FROM_WIN32(ERROR_BUSY);
        auto& buffer = plane.buffers[available];
        const auto animation = mpo::Animate(config_.demo, seconds, config_.speed,
            index, features_, plane.transformMask, plane.scaleMask);
        if (!mpo::Render({static_cast<std::uint8_t*>(buffer.pixels),
            static_cast<size_t>(buffer.pitch) * mpo::Height}, buffer.pitch,
            config_.demo, animation, seconds, index))
            return E_UNEXPECTED;
        Describe(index, available, seconds, &layers[index + 1]);
        plane.submittedLayer = layers[index + 1];
        auto upload = layers[index + 1];
        upload.skipUpload = NV_FALSE;
        upload.uploadSource = upload.surface;
        NV_KMS_WINDOWS_PRESENT_TIMING timing = {};
        const NV_STATUS copied = NvKmsWindowsUploadLayer(&upload, &timing);
        if (copied != NV_OK)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "MPO overlay upload failed: connector=%u plane=%u status=0x%08X",
                config_.connector, index + 1, copied);
            return E_FAIL;
        }
    }
    *count = config_.demoPlanes + 1;
    return S_OK;
}

HRESULT MpoScene::Submitted() noexcept
{
    for (unsigned index = 0; index < config_.demoPlanes; ++index)
    {
        const HRESULT queued = planes_[index].slots.Queue();
        if (FAILED(queued)) return queued;
    }
    if (preparedAnimation_ && !cadence_.Submitted(Clock100ns()))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "MPO cadence submission state invalid");
        return E_UNEXPECTED;
    }
    return S_OK;
}

} // namespace nv::iddcx
