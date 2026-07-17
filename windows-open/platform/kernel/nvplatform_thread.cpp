#include "nvplatform_thread.h"

extern "C" {
#include <ntifs.h>
}

namespace {

constexpr ULONG NvThreadPoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('T') << 16) |
    (static_cast<ULONG>('h') << 24);

struct ThreadState
{
    LIST_ENTRY listEntry;
    KEVENT startEvent;
    KEVENT completionEvent;
    PETHREAD threadObject;
    NV_PLATFORM_THREAD_MAIN threadMain;
    void* argument;
    NvU64 threadId;
    volatile LONG returnStatus;
};

FAST_MUTEX threadListMutex;
LIST_ENTRY threadList;
bool threadRegistryInitialized;
NV_PLATFORM_PROCESS_NOTIFY_ROUTINE processNotifyRoutine;
void* processNotifyContext;

ThreadState* FindThreadStateLocked(NvU64 threadId) noexcept
{
    for (PLIST_ENTRY entry = threadList.Flink;
         entry != &threadList;
         entry = entry->Flink)
    {
        auto* state = CONTAINING_RECORD(entry, ThreadState, listEntry);
        if (state->threadId == threadId)
        {
            return state;
        }
    }

    return nullptr;
}

void CompleteThread(ThreadState* state, NvS32 status) noexcept
{
    InterlockedExchange(&state->returnStatus, status);
    KeSetEvent(&state->completionEvent, IO_NO_INCREMENT, FALSE);
}

void ThreadStart(void* context)
{
    auto* state = static_cast<ThreadState*>(context);

    (void)KeWaitForSingleObject(
        &state->startEvent,
        Executive,
        KernelMode,
        FALSE,
        nullptr);
    CompleteThread(state, state->threadMain(state->argument));
}

PETHREAD ReferenceThread(NvU64 threadId) noexcept
{
    PETHREAD threadObject = nullptr;

    if (threadRegistryInitialized)
    {
        ExAcquireFastMutex(&threadListMutex);
        const ThreadState* state = FindThreadStateLocked(threadId);
        if (state != nullptr)
        {
            threadObject = state->threadObject;
            ObReferenceObject(threadObject);
        }
        ExReleaseFastMutex(&threadListMutex);
    }

    if (threadObject == nullptr)
    {
        (void)PsLookupThreadByThreadId(
            reinterpret_cast<HANDLE>(threadId),
            &threadObject);
    }

    return threadObject;
}

void ProcessNotify(
    PEPROCESS process,
    HANDLE processId,
    PPS_CREATE_NOTIFY_INFO createInfo)
{
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE notifyRoutine;
    void* context;

    UNREFERENCED_PARAMETER(process);

    ExAcquireFastMutex(&threadListMutex);
    notifyRoutine = processNotifyRoutine;
    context = processNotifyContext;
    ExReleaseFastMutex(&threadListMutex);

    if (notifyRoutine != nullptr)
    {
        notifyRoutine(
            reinterpret_cast<NvU64>(processId),
            createInfo != nullptr ? 0U : 1U,
            context);
    }
}

} // namespace

extern "C" NV_STATUS NvPlatformThreadInitialize(void)
{
    if (threadRegistryInitialized)
    {
        return NV_ERR_STATE_IN_USE;
    }

    ExInitializeFastMutex(&threadListMutex);
    InitializeListHead(&threadList);
    threadRegistryInitialized = true;
    return NV_OK;
}

extern "C" void NvPlatformThreadShutdown(void)
{
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE oldRoutine;
    void* oldContext;

    (void)NvPlatformThreadSetProcessNotifyRoutine(
        nullptr,
        nullptr,
        &oldRoutine,
        &oldContext);

    while (threadRegistryInitialized)
    {
        NvU64 threadId = 0;

        ExAcquireFastMutex(&threadListMutex);
        if (!IsListEmpty(&threadList))
        {
            const auto* state = CONTAINING_RECORD(
                threadList.Flink,
                ThreadState,
                listEntry);
            threadId = state->threadId;
        }
        ExReleaseFastMutex(&threadListMutex);

        if (threadId == 0)
        {
            threadRegistryInitialized = false;
            return;
        }

        NvPlatformThreadDestroy(threadId);
    }
}

extern "C" NV_STATUS NvPlatformThreadCreate(
    NvU64* threadId,
    NV_PLATFORM_THREAD_MAIN threadMain,
    void* argument)
{
    HANDLE threadHandle = nullptr;
    PETHREAD threadObject = nullptr;

    if (!threadRegistryInitialized || threadId == nullptr || threadMain == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *threadId = 0;
    auto* state = static_cast<ThreadState*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(ThreadState),
        NvThreadPoolTag));
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    KeInitializeEvent(&state->startEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&state->completionEvent, NotificationEvent, FALSE);
    state->threadMain = threadMain;
    state->argument = argument;

    NTSTATUS status = PsCreateSystemThread(
        &threadHandle,
        0,
        nullptr,
        nullptr,
        nullptr,
        ThreadStart,
        state);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(state, NvThreadPoolTag);
        return status == STATUS_INSUFFICIENT_RESOURCES ?
            NV_ERR_NO_MEMORY : NV_ERR_OPERATING_SYSTEM;
    }

    status = ObReferenceObjectByHandle(
        threadHandle,
        0,
        *PsThreadType,
        KernelMode,
        reinterpret_cast<void**>(&threadObject),
        nullptr);
    ZwClose(threadHandle);
    if (!NT_SUCCESS(status))
    {
        state->threadMain = [](void*) -> NvS32 { return -1; };
        KeSetEvent(&state->startEvent, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(
            &state->completionEvent,
            Executive,
            KernelMode,
            FALSE,
            nullptr);
        ExFreePoolWithTag(state, NvThreadPoolTag);
        return NV_ERR_OPERATING_SYSTEM;
    }

    state->threadObject = threadObject;
    state->threadId = reinterpret_cast<NvU64>(PsGetThreadId(threadObject));

    ExAcquireFastMutex(&threadListMutex);
    InsertTailList(&threadList, &state->listEntry);
    ExReleaseFastMutex(&threadListMutex);

    *threadId = state->threadId;
    KeSetEvent(&state->startEvent, IO_NO_INCREMENT, FALSE);
    return NV_OK;
}

extern "C" void NvPlatformThreadDestroy(NvU64 threadId)
{
    ThreadState* state;

    if (!threadRegistryInitialized)
    {
        return;
    }

    ExAcquireFastMutex(&threadListMutex);
    state = FindThreadStateLocked(threadId);
    ExReleaseFastMutex(&threadListMutex);
    if (state == nullptr)
    {
        return;
    }

    (void)KeWaitForSingleObject(
        &state->completionEvent,
        Executive,
        KernelMode,
        FALSE,
        nullptr);

    ExAcquireFastMutex(&threadListMutex);
    RemoveEntryList(&state->listEntry);
    ExReleaseFastMutex(&threadListMutex);

    ObDereferenceObject(state->threadObject);
    ExFreePoolWithTag(state, NvThreadPoolTag);
}

extern "C" void NvPlatformThreadExit(NvS32 status)
{
    ThreadState* state;
    const NvU64 threadId = NvPlatformGetCurrentThreadId();

    ExAcquireFastMutex(&threadListMutex);
    state = FindThreadStateLocked(threadId);
    ExReleaseFastMutex(&threadListMutex);
    if (state != nullptr)
    {
        CompleteThread(state, status);
        PsTerminateSystemThread(static_cast<NTSTATUS>(status));
    }
}

extern "C" NV_STATUS NvPlatformThreadWait(
    NvU64 threadId,
    NvS32* returnStatus)
{
    ThreadState* state;

    if (!threadRegistryInitialized)
    {
        return NV_ERR_INVALID_STATE;
    }

    ExAcquireFastMutex(&threadListMutex);
    state = FindThreadStateLocked(threadId);
    ExReleaseFastMutex(&threadListMutex);
    if (state == nullptr)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    (void)KeWaitForSingleObject(
        &state->completionEvent,
        Executive,
        KernelMode,
        FALSE,
        nullptr);
    if (returnStatus != nullptr)
    {
        *returnStatus = state->returnStatus;
    }

    return NV_OK;
}

extern "C" NvU64 NvPlatformThreadGetPriority(NvU64 threadId)
{
    PETHREAD threadObject = ReferenceThread(threadId);
    if (threadObject == nullptr)
    {
        return 0;
    }

    const KPRIORITY priority = KeQueryPriorityThread(threadObject);
    ObDereferenceObject(threadObject);
    return static_cast<NvU64>(priority);
}

extern "C" void NvPlatformThreadSetPriority(NvU64 threadId, NvU64 priority)
{
    PETHREAD threadObject = ReferenceThread(threadId);
    if (threadObject != nullptr)
    {
        (void)KeSetPriorityThread(
            threadObject,
            static_cast<KPRIORITY>(priority));
        ObDereferenceObject(threadObject);
    }
}

extern "C" NV_STATUS NvPlatformThreadSetAffinity(
    NvU64 group,
    NvU64 mask,
    NvU64* previousGroup,
    NvU64* previousMask)
{
    if (previousGroup == nullptr || previousMask == nullptr ||
        group > MAXUSHORT || mask == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_IRQ_LEVEL;
    }

    GROUP_AFFINITY affinity{};
    GROUP_AFFINITY previousAffinity{};
    affinity.Group = static_cast<USHORT>(group);
    affinity.Mask = static_cast<KAFFINITY>(mask);
    KeSetSystemGroupAffinityThread(&affinity, &previousAffinity);
    *previousGroup = previousAffinity.Group;
    *previousMask = previousAffinity.Mask;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformThreadSetAffinityByIndex(
    NvU32 coreIndex,
    NvU64* previousGroup,
    NvU64* previousMask)
{
    PROCESSOR_NUMBER processorNumber{};
    const NTSTATUS status = KeGetProcessorNumberFromIndex(
        coreIndex,
        &processorNumber);
    if (!NT_SUCCESS(status))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NvPlatformThreadSetAffinity(
        processorNumber.Group,
        1ULL << processorNumber.Number,
        previousGroup,
        previousMask);
}

extern "C" NV_STATUS NvPlatformThreadRevertAffinity(
    NvU64 previousGroup,
    NvU64 previousMask)
{
    if (previousGroup > MAXUSHORT || previousMask == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_IRQ_LEVEL;
    }

    GROUP_AFFINITY previousAffinity{};
    previousAffinity.Group = static_cast<USHORT>(previousGroup);
    previousAffinity.Mask = static_cast<KAFFINITY>(previousMask);
    KeRevertToUserGroupAffinityThread(&previousAffinity);
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformThreadSetProcessNotifyRoutine(
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE notifyRoutine,
    void* context,
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE* previousNotifyRoutine,
    void** previousContext)
{
    if (!threadRegistryInitialized || previousNotifyRoutine == nullptr ||
        previousContext == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    ExAcquireFastMutex(&threadListMutex);
    *previousNotifyRoutine = processNotifyRoutine;
    *previousContext = processNotifyContext;
    const bool wasRegistered = processNotifyRoutine != nullptr;
    ExReleaseFastMutex(&threadListMutex);

    if (!wasRegistered && notifyRoutine != nullptr)
    {
        ExAcquireFastMutex(&threadListMutex);
        processNotifyRoutine = notifyRoutine;
        processNotifyContext = context;
        ExReleaseFastMutex(&threadListMutex);

        const NTSTATUS status = PsSetCreateProcessNotifyRoutineEx(
            ProcessNotify,
            FALSE);
        if (!NT_SUCCESS(status))
        {
            ExAcquireFastMutex(&threadListMutex);
            processNotifyRoutine = *previousNotifyRoutine;
            processNotifyContext = *previousContext;
            ExReleaseFastMutex(&threadListMutex);
            return NV_ERR_OPERATING_SYSTEM;
        }
        return NV_OK;
    }
    else if (wasRegistered && notifyRoutine == nullptr)
    {
        const NTSTATUS status = PsSetCreateProcessNotifyRoutineEx(
            ProcessNotify,
            TRUE);
        if (!NT_SUCCESS(status))
        {
            return NV_ERR_OPERATING_SYSTEM;
        }
    }

    ExAcquireFastMutex(&threadListMutex);
    processNotifyRoutine = notifyRoutine;
    processNotifyContext = context;
    ExReleaseFastMutex(&threadListMutex);
    return NV_OK;
}

extern "C" NvU64 NvPlatformGetCurrentThreadId(void)
{
    return reinterpret_cast<NvU64>(PsGetCurrentThreadId());
}

extern "C" NvU64 NvPlatformGetCurrentProcessId(void)
{
    return reinterpret_cast<NvU64>(PsGetCurrentProcessId());
}

extern "C" NvU32 NvPlatformGetCurrentProcessFlags(void)
{
    constexpr NvU32 KernelThread = 0x1;
    constexpr NvU32 Exiting = 0x2;
    NvU32 flags = 0;

    if (PsIsSystemThread(PsGetCurrentThread()))
    {
        flags |= KernelThread;
    }

    if (PsIsThreadTerminating(PsGetCurrentThread()))
    {
        flags |= Exiting;
    }

    return flags;
}

extern "C" NvBool NvPlatformIsThreadIdValid(NvU64 threadId)
{
    PETHREAD thread = nullptr;

    if (threadId == 0 || KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_FALSE;
    }

    if (threadRegistryInitialized)
    {
        ExAcquireFastMutex(&threadListMutex);
        const bool found = FindThreadStateLocked(threadId) != nullptr;
        ExReleaseFastMutex(&threadListMutex);
        if (found)
        {
            return NV_TRUE;
        }
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
    (void)NvPlatformScheduleThread();
}

extern "C" NV_STATUS NvPlatformScheduleThread(void)
{
    LARGE_INTEGER interval{};
    interval.QuadPart = -1;

    if (KeGetCurrentIrql() <= APC_LEVEL)
    {
        (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
        return NV_OK;
    }

    return NV_ERR_INVALID_STATE;
}
