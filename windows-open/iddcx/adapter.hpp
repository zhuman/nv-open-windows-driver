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

private:
    enum class State : UCHAR
    {
        Stopped,
        Initializing,
        Ready,
        Failed,
    };

    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;

    IDDCX_ADAPTER handle_;
    NTSTATUS initializationStatus_;
    State state_;
    bool powered_;
};

EVT_IDD_CX_ADAPTER_INIT_FINISHED AdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES AdapterCommitModes;

} // namespace nv::iddcx