#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "nvkms_windows.h"

namespace nv::iddcx {

class Adapter;
class Output;

struct AdapterObjectContext final
{
    Adapter* owner;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterObjectContext, GetAdapterObjectContext);

struct ModePath final
{
    IDDCX_MONITOR monitor;
    IDDCX_PATH_FLAGS flags;
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signalInfo;
    NvU8 colorBpc;
    NV_KMS_WINDOWS_COLOR_SPACE colorSpace;
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
    bool IsReady() const noexcept;
    NTSTATUS RefreshDisplays() noexcept;
    IDDCX_ADAPTER Handle() const noexcept { return handle_; }
    WDFDEVICE Device() const noexcept { return device_; }
    void OnOutputDestroyed(_In_ Output* output) noexcept;

    NTSTATUS CompleteInitialization(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* arguments) noexcept;

    NTSTATUS CommitModes(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_COMMITMODES* arguments) noexcept;
    NTSTATUS CommitModes2(
        _In_ IDDCX_ADAPTER adapter,
        _In_ const IDARG_IN_COMMITMODES2* arguments) noexcept;

private:
    enum class State : UCHAR
    {
        Stopped,
        Initializing,
        Ready,
        Failed,
    };

    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    NTSTATUS TryAnnounceMonitors() noexcept;
    void ResetOutputs() noexcept;
    NTSTATUS CommitDisplayModes(
        _In_reads_(pathCount) const ModePath* paths,
        _In_ UINT pathCount) noexcept;

    WDFDEVICE device_;
    IDDCX_ADAPTER handle_;
    Output* outputs_[MaxOutputs];
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

} // namespace nv::iddcx