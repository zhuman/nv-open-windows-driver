#pragma once

#include "output.hpp"
#include "mpo_patterns.hpp"
#include "frame_slots.hpp"
#include <array>

namespace nv::iddcx {

class MpoScene final
{
public:
    static HRESULT Create(Output& output, const SwapChainConfig& config,
        std::unique_ptr<MpoScene>* scene) noexcept;
    HRESULT Retire(bool* pending) noexcept;
    HRESULT Prepare(const NV_KMS_WINDOWS_LAYER& primary,
        NV_KMS_WINDOWS_LAYER* layers, NvU32* count) noexcept;
    HRESULT Submitted() noexcept;
    bool Due() const noexcept;
    void Restart() noexcept { cadence_.Reset(); }
    ULONGLONG Wait100ns() const noexcept { return cadence_.Remaining(Clock100ns()); }
    ULONG PlaneCount() const noexcept { return config_.demoPlanes; }
    bool RotationSupported() const noexcept { return caps_.rotationMask == 15; }
    bool LutSupported() const noexcept { return caps_.inputLutSupported != 0; }

private:
    struct Buffer
    {
        unique_nvkms_surface surface;
        void* pixels = nullptr;
        NvU32 pitch = 0;
    };
    struct Plane
    {
        Buffer buffers[3];
        unique_nvkms_surface luts[mpo::LutCount];
        NV_KMS_WINDOWS_COLOR_TRANSFORM transform = {};
        NV_KMS_WINDOWS_LAYER submittedLayer = {};
        PlaneBufferSlots slots;
        NvU64 timestamp = 0;
        unsigned transformMask = 0;
        unsigned scaleMask = 2;
    };
    static constexpr UINT FrameInvalid = ~UINT{0};
    HRESULT Initialize() noexcept;
    ULONGLONG Clock100ns() const noexcept;
    void Describe(unsigned index, unsigned buffer, double seconds,
        NV_KMS_WINDOWS_LAYER* layer, unsigned animationMode = 0) noexcept;
    Output* output_ = nullptr;
    SwapChainConfig config_;
    NV_KMS_WINDOWS_LAYER_CAPS caps_ = {};
    unsigned width_ = 0;
    unsigned height_ = 0;
    unsigned features_ = mpo::AllFeatures;
    ULONGLONG started_ = 0;
    ULONGLONG frequency_ = 0;
    mpo::FrameCadence cadence_;
    bool preparedAnimation_ = false;
    std::array<Plane, 7> planes_;
};

} // namespace nv::iddcx
