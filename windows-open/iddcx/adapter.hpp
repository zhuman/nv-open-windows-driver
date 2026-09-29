#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

namespace nv::iddcx {

class Adapter;

struct AdapterObjectContext final
{
    Adapter* owner;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterObjectContext, GetAdapterObjectContext);

struct MonitorObjectContext final
{
    Adapter* owner;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorObjectContext, GetMonitorObjectContext);

class Adapter final
{
public:
    Adapter() noexcept;

    NTSTATUS EnterD0(_In_ WDFDEVICE device) noexcept;
    void ExitD0() noexcept;
    void Reset() noexcept;

    NTSTATUS CompleteInitialization(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* arguments) noexcept;

    NTSTATUS CommitModes(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_COMMITMODES* arguments) const noexcept;
    NTSTATUS CommitModes2(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_COMMITMODES2* arguments) const noexcept;

    NTSTATUS AssignSwapChain(
        _In_ IDDCX_MONITOR monitor,
        _In_ const IDARG_IN_SETSWAPCHAIN* arguments) noexcept;
    NTSTATUS UnassignSwapChain(_In_ IDDCX_MONITOR monitor) noexcept;

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
    NTSTATUS AnnounceMonitor() noexcept;
    void ProcessSwapChain() noexcept;
    void StopSwapChain() noexcept;

    IDDCX_ADAPTER handle_;
    IDDCX_MONITOR monitor_;
    IDDCX_SWAPCHAIN swapchain_;
    IDXGIDevice* swapchainDevice_;
    HANDLE swapchainThread_;
    HANDLE swapchainStopEvent_;
    HANDLE nextSurfaceEvent_;
    SRWLOCK swapchainLock_;
    NTSTATUS initializationStatus_;
    State state_;
    bool powered_;
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