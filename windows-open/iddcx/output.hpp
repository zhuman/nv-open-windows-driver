#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <wil/resource.h>

#include "nvkms_windows.h"

namespace nv::iddcx {

class Adapter;
class SwapChainD3D11;

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
        return scanoutSurface_.get();
    }
    void* ScanoutPixels() const noexcept { return scanoutPixels_; }
    NvU32 ScanoutPitch() const noexcept { return scanoutPitch_; }
    NvU32 ActiveModeIndex() const noexcept { return activeModeIndex_; }
    NvU32 AssignedHead() const noexcept { return assignedHead_; }
    bool Active() const noexcept { return active_; }
    auto LockFrameStateShared() noexcept
    {
        return stateLock_.lock_shared();
    }

    NTSTATUS PrepareScanout(
        const NV_KMS_WINDOWS_MODE& mode,
        NvU32 outputIndex,
        NV_KMS_WINDOWS_SURFACE** surface,
        void** pixels,
        NvU32* pitch) noexcept;
    void CommitMode(
        bool active,
        NvU32 modeIndex,
        NV_KMS_WINDOWS_SURFACE* newSurface,
        void* newPixels,
        NvU32 newPitch) noexcept;
    void SetAssignedHead(NvU32 head) noexcept;
    NTSTATUS AssignSwapChain(
        IDDCX_MONITOR monitor,
        const IDARG_IN_SETSWAPCHAIN* arguments) noexcept;
    NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor) noexcept;
    void OnSwapChainDestroyed(SwapChainD3D11* swapChain) noexcept;

private:
    Output(
        Adapter& owner,
        IDDCX_MONITOR monitor,
        const NV_KMS_WINDOWS_DISPLAY_STATE& state) noexcept;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    void StopSwapChain() noexcept;
    void DestroyScanout() noexcept;
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    Adapter* owner_;
    NV_KMS_WINDOWS_DISPLAY_STATE displayState_;
    IDDCX_MONITOR monitor_;
    unique_nvkms_surface scanoutSurface_;
    void* scanoutPixels_;
    NvU32 scanoutPitch_;
    NvU32 activeModeIndex_;
    NvU32 assignedHead_;
    bool active_;
    wil::srwlock stateLock_;
    SwapChainD3D11* swapChain_;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Output, GetOutput);

EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES GetDefaultDescriptionModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES QueryTargetModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 QueryTargetModes2;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN AssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN UnassignSwapChain;

} // namespace nv::iddcx
