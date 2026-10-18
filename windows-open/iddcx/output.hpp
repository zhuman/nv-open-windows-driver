#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <wil/resource.h>
#include <memory>
#include <atomic>

#include "nvkms_windows.h"
#include "driver_parameters.hpp"

namespace nv::iddcx {

class Adapter;
class SwapChainD3D11;
class SwapChainD3D12;
class HwCursor;
class MpoScene;

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
        return Surface(frontBufferIndex_.load());
    }
    void* ScanoutPixels() const noexcept
    {
        return Pixels(frontBufferIndex_.load());
    }
    NV_KMS_WINDOWS_SURFACE* BackSurface() const noexcept
    {
        return Surface(copyBufferIndex_.load());
    }
    void* BackPixels() const noexcept
    {
        return Pixels(copyBufferIndex_.load());
    }
    NvU32 BackBufferIndex() const noexcept { return copyBufferIndex_.load(); }
    NV_KMS_WINDOWS_SURFACE* Surface(NvU32 index) const noexcept
    {
        return index == 0 ? scanoutSurface_.get() : index == 1 ?
            backSurface_.get() : extraSurfaces_[index - 2].get();
    }
    void* Pixels(NvU32 index) const noexcept
    {
        return index == 0 ? scanoutPixels_ : index == 1 ? backPixels_ : extraPixels_[index - 2];
    }
    NvU32 FrontBufferIndex() const noexcept { return frontBufferIndex_.load(); }
    void SelectCopyBuffer(NvU32 index) noexcept { copyBufferIndex_.store(index); }
    void SetFrontBuffer(NvU32 index) noexcept { frontBufferIndex_.store(index); }
    NvU32 SubmittedBufferIndex() const noexcept { return submittedBufferIndex_.load(); }
    void SetSubmittedBuffer(NvU32 index) noexcept { submittedBufferIndex_.store(index); }
    bool RequiresScanoutReset() const noexcept { return scanoutRequiresReset_.load(); }
    void RequireScanoutReset() noexcept { scanoutRequiresReset_.store(true); }
    const PresentationPolicy& PresentPolicy() const noexcept { return presentationPolicy_; }
    const SwapChainConfig& Config() const noexcept { return config_; }
    MpoScene* DemoScene() const noexcept { return scene_.get(); }
    HRESULT CheckConfiguration() noexcept;
    HRESULT ValidateConfiguration(const SwapChainConfig& config) noexcept;
    void RetirePreviousPool() noexcept;
    void PresentationApplied() noexcept;
    bool VrrEnabled() const noexcept { return vrrEnabled_; }
    void SetVrrEnabled(bool enabled) noexcept { vrrEnabled_ = enabled; }
    NTSTATUS ConfigurePresentationPolicy() noexcept;
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
    bool HardwareRemoved() const noexcept;
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
    void OnSwapChainDestroyed(SwapChainD3D12* swapChain) noexcept;
    NTSTATUS SetDefaultHdrMetadata(
        const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* arguments) noexcept;
    NTSTATUS SetGammaRamp(const IDARG_IN_SET_GAMMARAMP* arguments) noexcept;
    NTSTATUS MonitorI2c(
        NvU32 transmitAddress, NvU32 transmitSize, const NvU8* transmit,
        NvU32 receiveAddress, NvU32 receiveSize, NvU8* receive,
        NvU32 receiveFlags) noexcept;
    void StopMonitorControl() noexcept { monitorControlAvailable_.store(false); }

private:
    Output(
        Adapter& owner,
        IDDCX_MONITOR monitor,
        const NV_KMS_WINDOWS_DISPLAY_STATE& state) noexcept;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    void DestroyScanout() noexcept;
    NTSTATUS StageConfiguration(const SwapChainConfig& config) noexcept;
    NTSTATUS ApplyConfiguration() noexcept;
    NTSTATUS ValidateVrr(const SwapChainConfig& config) noexcept;
    NTSTATUS DrainPresentation() noexcept;
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    Adapter* owner_;
    NV_KMS_WINDOWS_DISPLAY_STATE displayState_;
    IDDCX_MONITOR monitor_;
    unique_nvkms_surface scanoutSurface_;
    unique_nvkms_surface backSurface_;
    void* backPixels_ = nullptr;
    std::atomic<NvU32> frontBufferIndex_{0};
    std::atomic<NvU32> copyBufferIndex_{1};
    std::atomic<NvU32> submittedBufferIndex_{0};
    std::atomic<bool> scanoutRequiresReset_{false};
    unique_nvkms_surface extraSurfaces_[2];
    void* extraPixels_[2] = {};
    PresentationPolicy presentationPolicy_;
    SwapChainConfig config_;
    SwapChainConfig pendingConfig_;
    bool havePendingConfig_ = false;
    ULONG checkedGeneration_ = 0;
    std::atomic<ULONG> appliedGeneration_{0};
    std::atomic<ULONG> requestedGeneration_{0};
    std::atomic<bool> presentationVisible_{false};
    struct ScanoutPool
    {
        unique_nvkms_surface surfaces[4];
        void* pixels[4] = {};
        NvU32 pitch = 0;
    };
    std::unique_ptr<ScanoutPool> pendingPool_;
    unique_nvkms_surface retiredFront_;
    std::atomic<bool> retiredFrontPending_{false};
    std::unique_ptr<MpoScene> scene_;
    std::unique_ptr<MpoScene> pendingScene_;
    bool vrrEnabled_ = false;
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
    SwapChainD3D12* swapChain12_ = nullptr;
    bool d3d12Failed_ = false;
    std::atomic<bool> monitorControlAvailable_{true};
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
EVT_IDD_CX_MONITOR_I2C_TRANSMIT MonitorI2cTransmit;
EVT_IDD_CX_MONITOR_I2C_RECEIVE MonitorI2cReceive;
EVT_IDD_CX_MONITOR_I2C_TRANSMIT_AND_RECEIVE MonitorI2cTransmitAndReceive;

} // namespace nv::iddcx
