#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <wil/resource.h>
#include <memory>
#include <atomic>

#include "nvkms_windows.h"

namespace nv::iddcx {

class Adapter;
class SwapChainD3D11;
class HwCursor;

using unique_nvkms_surface = wil::unique_any<
    NV_KMS_WINDOWS_SURFACE*,
    decltype(&NvKmsWindowsDestroyScanoutSurface),
    NvKmsWindowsDestroyScanoutSurface>;

class Output final
{
public:
    static NTSTATUS Create(
        Adapter& owner,
        const NV_KMS_WINDOWS_DISPLAY_STATE& state,
        Output** output) noexcept;
    ~Output() noexcept;

    Adapter& Owner() const noexcept { return *owner_; }
    IDDCX_MONITOR Monitor() const noexcept { return monitor_; }
    const NV_KMS_WINDOWS_DISPLAY_STATE& DisplayState() const noexcept { return displayState_; }
    // Mutable frame-state accessors require a shared guard during normal operation.
    NV_KMS_WINDOWS_SURFACE* ScanoutSurface() const noexcept
    {
        return frontBufferIndex_.load() == 0 ? scanoutSurface_.get() : backSurface_.get();
    }
    void* ScanoutPixels() const noexcept
    {
        return frontBufferIndex_.load() == 0 ? scanoutPixels_ : backPixels_;
    }
    NV_KMS_WINDOWS_SURFACE* BackSurface() const noexcept
    {
        return frontBufferIndex_.load() == 0 ? backSurface_.get() : scanoutSurface_.get();
    }
    void* BackPixels() const noexcept
    {
        return frontBufferIndex_.load() == 0 ? backPixels_ : scanoutPixels_;
    }
    NvU32 BackBufferIndex() const noexcept { return frontBufferIndex_.load() ^ 1U; }
    void CommitPresent() noexcept { frontBufferIndex_.fetch_xor(1U); }
    NTSTATUS PrepareBackBuffer() noexcept;
    NvU32 ScanoutPitch() const noexcept { return scanoutPitch_; }
    NvU32 ActiveModeIndex() const noexcept { return activeModeIndex_; }
    NvU32 AssignedHead() const noexcept { return assignedHead_; }
    NV_KMS_WINDOWS_SCANOUT_FORMAT ScanoutFormat() const noexcept
    {
        return scanoutFormat_;
    }
    NvU8 ColorBpc() const noexcept { return colorBpc_; }
    NV_KMS_WINDOWS_COLOR_SPACE ColorSpace() const noexcept { return colorSpace_; }
    const NV_KMS_WINDOWS_HDR_METADATA& DefaultHdrMetadata() const noexcept
    {
        return defaultHdrMetadata_;
    }
    bool HasDefaultHdrMetadata() const noexcept { return hasDefaultHdrMetadata_; }
    const NV_KMS_WINDOWS_COLOR_TRANSFORM& ColorTransform() const noexcept
    {
        return colorTransform_;
    }
    bool Active() const noexcept { return active_; }
    auto LockFrameStateShared() noexcept
    {
        return stateLock_.lock_shared();
    }

    NTSTATUS PrepareScanout(
        const NV_KMS_WINDOWS_MODE& mode,
        NvU32 outputIndex,
        NV_KMS_WINDOWS_SCANOUT_FORMAT format,
        NV_KMS_WINDOWS_SURFACE** surface,
        void** pixels,
        NvU32* pitch) noexcept;
    void CommitMode(
        bool active,
        NvU32 modeIndex,
        NV_KMS_WINDOWS_SURFACE* newSurface,
        void* newPixels,
        NvU32 newPitch,
        NV_KMS_WINDOWS_SCANOUT_FORMAT format,
        NvU8 colorBpc,
        NV_KMS_WINDOWS_COLOR_SPACE colorSpace) noexcept;
    void SetAssignedHead(NvU32 head) noexcept;
    NTSTATUS AssignSwapChain(
        IDDCX_MONITOR monitor,
        const IDARG_IN_SETSWAPCHAIN* arguments) noexcept;
    NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor) noexcept;
    void QuiesceSwapChain() noexcept;
    void PauseCursor() noexcept;
    void ResumeCursor() noexcept;
    void OnSwapChainDestroyed(SwapChainD3D11* swapChain) noexcept;
    NTSTATUS SetDefaultHdrMetadata(
        const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* arguments) noexcept;
    NTSTATUS SetGammaRamp(const IDARG_IN_SET_GAMMARAMP* arguments) noexcept;

private:
    Output(
        Adapter& owner,
        IDDCX_MONITOR monitor,
        const NV_KMS_WINDOWS_DISPLAY_STATE& state) noexcept;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    void DestroyScanout() noexcept;
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    Adapter* owner_;
    NV_KMS_WINDOWS_DISPLAY_STATE displayState_;
    IDDCX_MONITOR monitor_;
    unique_nvkms_surface scanoutSurface_;
    unique_nvkms_surface backSurface_;
    void* backPixels_ = nullptr;
    std::atomic<NvU32> frontBufferIndex_{0};
    void* scanoutPixels_;
    NvU32 scanoutPitch_;
    NvU32 activeModeIndex_;
    NvU32 assignedHead_;
    NV_KMS_WINDOWS_SCANOUT_FORMAT scanoutFormat_;
    NvU8 colorBpc_;
    NV_KMS_WINDOWS_COLOR_SPACE colorSpace_;
    NV_KMS_WINDOWS_HDR_METADATA defaultHdrMetadata_;
    bool hasDefaultHdrMetadata_;
    NV_KMS_WINDOWS_COLOR_TRANSFORM colorTransform_;
    bool active_;
    wil::srwlock stateLock_;
    SwapChainD3D11* swapChain_;
    std::unique_ptr<HwCursor> cursor_;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Output, GetOutput);

EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES GetDefaultDescriptionModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES QueryTargetModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 QueryTargetModes2;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN AssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN UnassignSwapChain;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA SetDefaultHdrMetadata;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP SetGammaRamp;

} // namespace nv::iddcx
