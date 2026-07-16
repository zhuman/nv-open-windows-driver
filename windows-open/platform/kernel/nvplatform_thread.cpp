#include "nvplatform_thread.h"

extern "C" {
#include <ntifs.h>
}

extern "C" NvU64 NvPlatformGetCurrentThreadId(void)
{
    return reinterpret_cast<NvU64>(PsGetCurrentThreadId());
}

extern "C" NvU64 NvPlatformGetCurrentProcessId(void)
{
    return reinterpret_cast<NvU64>(PsGetCurrentProcessId());
}

extern "C" NvBool NvPlatformIsThreadIdValid(NvU64 threadId)
{
    PETHREAD thread = nullptr;

    if (threadId == 0 || KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_FALSE;
    }

    const NTSTATUS status = PsLookupThreadByThreadId(
        reinterpret_cast<HANDLE>(threadId),
        &thread);
    if (!NT_SUCCESS(status))
    {
        return NV_FALSE;
    }

    ObDereferenceObject(thread);
    return NV_TRUE;
}

extern "C" void NvPlatformYieldThread(void)
{
    LARGE_INTEGER interval{};

    if (KeGetCurrentIrql() <= APC_LEVEL)
    {
        (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
}
