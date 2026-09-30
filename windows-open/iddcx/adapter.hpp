#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "nvkms_windows.h"

namespace nv::iddcx {

class Adapter;
struct OutputState;

struct AdapterObjectContext final
{
    Adapter* owner;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterObjectContext, GetAdapterObjectContext);

struct MonitorObjectContext final
{
    Adapter* owner;
    OutputState* output;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorObjectContext, GetMonitorObjectContext);

struct OutputState final
{
    Adapter* owner;
    NV_KMS_WINDOWS_DISPLAY_STATE displayState;
    IDDCX_MONITOR monitor;
    IDDCX_SWAPCHAIN swapchain;
    IDXGIDevice* swapchainDevice;
    HANDLE swapchainThread;
    HANDLE swapchainStopEvent;
    HANDLE nextSurfaceEvent;
    SRWLOCK swapchainLock;
    NV_KMS_WINDOWS_SURFACE* scanoutSurface;
    void* scanoutPixels;
    NvU32 scanoutPitch;
    NvU32 activeModeIndex;
    NvU32 assignedHead;
    bool active;
};

struct ModePath final
{
    IDDCX_MONITOR monitor;
    IDDCX_PATH_FLAGS flags;
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signalInfo;
};

class Adapter final
{
public:
    static constexpr NvU32 MaxOutputs = 16;

    Adapter() noexcept;

    NTSTATUS EnterD0(_In_ WDFDEVICE device) noexcept;
    NTSTATUS NotifyDisplayReady() noexcept;
    void ExitD0() noexcept;
    void Reset() noexcept;

    NTSTATUS CompleteInitialization(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* arguments) noexcept;

    NTSTATUS CommitModes(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_COMMITMODES* arguments) noexcept;
    NTSTATUS CommitModes2(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_COMMITMODES2* arguments) noexcept;

    NTSTATUS AssignSwapChain(
        _Inout_ OutputState* output,
        _In_ IDDCX_MONITOR monitor,
        _In_ const IDARG_IN_SETSWAPCHAIN* arguments) noexcept;
    NTSTATUS UnassignSwapChain(
        _Inout_ OutputState* output,
        _In_ IDDCX_MONITOR monitor) noexcept;

private:
    enum class State : UCHAR
    {
        Stopped,
        Initializing,
        Ready,
        Failed,
    };

    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    static DWORD WINAPI SwapChainThread(_In_ void* context);
    NTSTATUS AnnounceMonitor(_Inout_ OutputState* output) noexcept;
    NTSTATUS TryAnnounceMonitors() noexcept;
    void ProcessSwapChain(_Inout_ OutputState* output) noexcept;
    void StopSwapChain(_Inout_ OutputState* output) noexcept;
    void ResetOutputs() noexcept;
    NTSTATUS CommitDisplayModes(
        _In_reads_(pathCount) const ModePath* paths,
        _In_ UINT pathCount) noexcept;

    IDDCX_ADAPTER handle_;
    OutputState* outputs_[MaxOutputs];
    NvU32 outputCount_;
    NTSTATUS initializationStatus_;
    State state_;
    bool powered_;
    volatile LONG displayReady_;
};

EVT_IDD_CX_ADAPTER_INIT_FINISHED AdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES AdapterCommitModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 AdapterCommitModes2;
EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO AdapterQueryTargetInfo;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION ParseMonitorDescription;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 ParseMonitorDescription2;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES GetDefaultDescriptionModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES QueryTargetModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 QueryTargetModes2;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN AssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN UnassignSwapChain;

} // namespace nv::iddcx