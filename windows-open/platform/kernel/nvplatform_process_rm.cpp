#include <ntifs.h>

#include "nvplatform_process_rm.h"

namespace {

constexpr ULONG ProcessStatePoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('P') << 16) |
    (static_cast<ULONG>('s') << 24);

struct ProcessState
{
    KAPC_STATE apcState;
    PEPROCESS process;
};

} // namespace

extern "C" NV_STATUS NvPlatformRmAttachToProcess(
    void** processState,
    NvU32 processId)
{
    if (processState == nullptr || processId == 0 || KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *processState = nullptr;
    auto* state = static_cast<ProcessState*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(ProcessState),
        ProcessStatePoolTag));
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    RtlZeroMemory(state, sizeof(*state));
    const NTSTATUS status = PsLookupProcessByProcessId(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(processId)),
        &state->process);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(state, ProcessStatePoolTag);
        return status == STATUS_INVALID_CID
            ? NV_ERR_OBJECT_NOT_FOUND
            : NV_ERR_OPERATING_SYSTEM;
    }

    KeStackAttachProcess(state->process, &state->apcState);
    *processState = state;
    return NV_OK;
}

extern "C" void NvPlatformRmDetachFromProcess(void* processState)
{
    auto* state = static_cast<ProcessState*>(processState);
    if (state == nullptr)
    {
        return;
    }

    KeUnstackDetachProcess(&state->apcState);
    ObDereferenceObject(state->process);
    ExFreePoolWithTag(state, ProcessStatePoolTag);
}

extern "C" NV_STATUS NvPlatformRmValidateKernelBuffer(
    const void* address,
    NvU32 size)
{
    if (size == 0)
    {
        return NV_OK;
    }

    const ULONG_PTR start = reinterpret_cast<ULONG_PTR>(address);
    const ULONG_PTR systemStart = reinterpret_cast<ULONG_PTR>(MmSystemRangeStart);
    if (start < systemStart || size - 1 > MAXULONG_PTR - start)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    return NV_OK;
}