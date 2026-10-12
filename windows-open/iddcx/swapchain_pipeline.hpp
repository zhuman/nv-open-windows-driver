#pragma once

#include "frame_slots.hpp"
#include "dirty_regions.hpp"
#include <wdf.h>
#include "nvkms_windows.h"
#include <d3d12.h>
#include <atomic>
#include <wil/resource.h>

namespace nv::iddcx {

class Output;
class MpoScene;

class SwapChainUploadPipeline final
{
public:
    ~SwapChainUploadPipeline() noexcept;
    HRESULT Start(Output& output, HANDLE stopEvent, NvU32 sourceApi) noexcept;
    HRESULT Acquire(UINT* index) noexcept;
    HRESULT Submit(UINT index, NvU64 frame, const NV_KMS_WINDOWS_LAYER& layer,
        ID3D12Resource* mapped, NvU64 sourceStart, NvU64 sourceEnd,
        NvU64 gpuSubmit, NvU64 gpuComplete) noexcept;
    HRESULT Stop() noexcept;
    HRESULT Poll() noexcept { return !enabled_ && !scene_ ? Refresh() : result_.load(); }
    bool Enabled() const noexcept { return enabled_; }

private:
    struct Job
    {
        UINT slot = FrameSlots::Invalid;
        NvU64 frame = 0;
        NV_KMS_WINDOWS_LAYER layer = {};
        NV_KMS_WINDOWS_COLOR_TRANSFORM transform = {};
        ID3D12Resource* mapped = nullptr;
        NvU64 sourceStart = 0;
        NvU64 sourceEnd = 0;
        NvU64 gpuSubmit = 0;
        NvU64 gpuComplete = 0;
    };
    static DWORD WINAPI ThreadEntry(void* context) noexcept;
    void Process() noexcept;
    HRESULT Refresh() noexcept;
    void TraceLayerRetirement() noexcept;
    HRESULT Consume(Job& job) noexcept;
    HRESULT AddDamage(const Job& job) noexcept;
    HRESULT WaitForLayers() noexcept;
    HRESULT Animate() noexcept;
    HRESULT DisableDemo() noexcept;
    void Unmap(Job& job) noexcept;
    void Fail(HRESULT result) noexcept;
    Output* output_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    bool enabled_ = false;
    NvU32 sourceApi_ = 0;
    bool submitted_ = false;
    MpoScene* scene_ = nullptr;
    bool demoSubmitted_ = false;
    NV_KMS_WINDOWS_LAYER desktop_ = {};
    NV_KMS_WINDOWS_COLOR_TRANSFORM desktopTransform_ = {};
    ULONGLONG lastSubmissionMs_ = 0;
    wil::unique_handle thread_;
    wil::unique_handle wake_;
    wil::unique_handle available_;
    wil::unique_handle animationTimer_;
    wil::srwlock lock_;
    wil::srwlock presentLock_;
    FrameSlots slots_;
    FrameRamSlots ramSlots_;
    DirtyRegionTracker vramDamage_;
    NvU64 queuedFrames_[4] = {};
    UINT width_ = 0;
    Job jobs_[4];
    UINT queue_[4] = {};
    UINT queueCount_ = 0;
    std::atomic<HRESULT> result_{S_OK};
};

} // namespace nv::iddcx
