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

extern "C" void* NvPlatformRmReferenceCurrentProcess(void)
{
    PEPROCESS process = PsGetCurrentProcess();
    ObReferenceObject(process);
    return process;
}

extern "C" void NvPlatformRmDereferenceProcess(void* process)
{
    if (process != nullptr)
    {
        ObDereferenceObject(process);
    }
}

extern "C" NV_STATUS NvPlatformRmGetProcessId(
    void* process,
    NvU32* processId)
{
    if (process == nullptr || processId == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const ULONG_PTR id = reinterpret_cast<ULONG_PTR>(
        PsGetProcessId(static_cast<PEPROCESS>(process)));
    if (id > MAXULONG)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    *processId = static_cast<NvU32>(id);
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetCurrentProcessId(NvU32* processId)
{
    return NvPlatformRmGetProcessId(PsGetCurrentProcess(), processId);
}

extern "C" NV_STATUS NvPlatformRmGetCurrentUserSid(
    NvU8* sid,
    NvU32* sidLength)
{
    if (sidLength == nullptr || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    PACCESS_TOKEN token = PsReferencePrimaryToken(PsGetCurrentProcess());
    PTOKEN_USER tokenUser = nullptr;
    const NTSTATUS status = SeQueryInformationToken(
        token,
        TokenUser,
        reinterpret_cast<void**>(&tokenUser));
    PsDereferencePrimaryToken(token);
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    const ULONG requiredLength = RtlLengthSid(tokenUser->User.Sid);
    NV_STATUS nvStatus = NV_OK;
    if (sid == nullptr || *sidLength < requiredLength)
    {
        *sidLength = requiredLength;
        nvStatus = NV_ERR_BUFFER_TOO_SMALL;
    }
    else
    {
        RtlCopyMemory(sid, tokenUser->User.Sid, requiredLength);
        *sidLength = requiredLength;
    }

    ExFreePool(tokenUser);
    return nvStatus;
}

extern "C" NvBool NvPlatformRmHasSystemProfilePrivilege(void)
{
    const LUID privilege = RtlConvertLongToLuid(SE_SYSTEM_PROFILE_PRIVILEGE);
    return SeSinglePrivilegeCheck(privilege, UserMode)
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" NvBool NvPlatformRmHasIncreaseBasePriorityPrivilege(void)
{
    const LUID privilege = RtlConvertLongToLuid(SE_INC_BASE_PRIORITY_PRIVILEGE);
    return SeSinglePrivilegeCheck(privilege, UserMode)
        ? NV_TRUE
        : NV_FALSE;
}