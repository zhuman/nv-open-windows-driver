#include "nvport/nvport.h"
#include "nvport/thread.h"

#include "nvplatform_thread.h"

const PORT_THREAD PORT_THREAD_INVALID = {0ULL};
const PORT_PROCESS PORT_PROCESS_INVALID = {0ULL};
const NvU64 portThreadPriorityMin = 0ULL;
const NvU64 portThreadPriorityDefault = 8ULL;
const NvU64 portThreadPriorityLowRealtime = 16ULL;
const NvU64 portThreadPriorityMax = 31ULL;

NvBool portThreadIsValid(PORT_THREAD thread)
{
    return NvPlatformIsThreadIdValid(thread.threadId);
}

PORT_THREAD portThreadGetCurrentThread(void)
{
    PORT_THREAD thread = {NvPlatformGetCurrentThreadId()};
    return thread;
}

PORT_THREAD portThreadGetThreadById(NvU64 threadId)
{
    PORT_THREAD thread = {threadId};
    return portThreadIsValid(thread) ? thread : PORT_THREAD_INVALID;
}

NvU64 portThreadGetCurrentThreadId(void)
{
    return NvPlatformGetCurrentThreadId();
}

NvU64 portThreadGetCurrentProcessId(void)
{
    return NvPlatformGetCurrentProcessId();
}

NvBool portThreadEqual(PORT_THREAD thread1, PORT_THREAD thread2)
{
    return thread1.threadId == thread2.threadId ? NV_TRUE : NV_FALSE;
}

NV_STATUS portThreadCreate(
    PORT_THREAD* thread,
    PORT_THREAD_MAIN threadMain,
    void* argument)
{
    if (thread == NULL)
    {
        return NV_ERR_INVALID_POINTER;
    }

    thread->threadId = 0;
    return NvPlatformThreadCreate(&thread->threadId, threadMain, argument);
}

void portThreadDestroy(PORT_THREAD thread)
{
    NvPlatformThreadDestroy(thread.threadId);
}

void portThreadExit(NvS32 status)
{
    NvPlatformThreadExit(status);
    PORT_BREAKPOINT_CHECKED();
}

NV_STATUS portThreadWaitToComplete(
    PORT_THREAD threadToWaitOn,
    NvS32* returnStatus)
{
    return NvPlatformThreadWait(threadToWaitOn.threadId, returnStatus);
}

void portThreadYield(void)
{
    NvPlatformYieldThread();
}

NvU64 portThreadExGetPriority(NvU64 threadId)
{
    return NvPlatformThreadGetPriority(threadId);
}

void portThreadExSetPriority(NvU64 threadId, NvU64 priority)
{
    NvPlatformThreadSetPriority(threadId, priority);
}

NV_STATUS portThreadExSetAffinity(
    const PORT_THREAD_AFFINITY* affinity,
    PORT_THREAD_AFFINITY* previousAffinity)
{
    if (affinity == NULL || previousAffinity == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NvPlatformThreadSetAffinity(
        affinity->cpuGroup,
        affinity->mask,
        &previousAffinity->cpuGroup,
        &previousAffinity->mask);
}

NV_STATUS portThreadExSetSystemGroupAffinity(
    NvU32 coreIndex,
    PORT_THREAD_AFFINITY* previousAffinity)
{
    if (previousAffinity == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NvPlatformThreadSetAffinityByIndex(
        coreIndex,
        &previousAffinity->cpuGroup,
        &previousAffinity->mask);
}

NV_STATUS portThreadExRevertToUserGroupAffinity(
    PORT_THREAD_AFFINITY* previousAffinity)
{
    if (previousAffinity == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NvPlatformThreadRevertAffinity(
        previousAffinity->cpuGroup,
        previousAffinity->mask);
}

NV_STATUS portThreadExRegisterProcessNotifyRoutine(
    PORT_THREAD_PROCESS_NOTIFY_ROUTINE notifyRoutine,
    void* context,
    PORT_THREAD_PROCESS_NOTIFY_ROUTINE* previousNotifyRoutine,
    void** previousContext)
{
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE platformPreviousRoutine;
    NV_STATUS status;

    if (previousNotifyRoutine == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = NvPlatformThreadSetProcessNotifyRoutine(
        (NV_PLATFORM_PROCESS_NOTIFY_ROUTINE)notifyRoutine,
        context,
        &platformPreviousRoutine,
        previousContext);
    if (status == NV_OK)
    {
        *previousNotifyRoutine =
            (PORT_THREAD_PROCESS_NOTIFY_ROUTINE)platformPreviousRoutine;
    }

    return status;
}

NV_STATUS portThreadExUnregisterProcessNotifyRoutine(
    PORT_THREAD_PROCESS_NOTIFY_ROUTINE previousNotifyRoutine,
    void* previousContext)
{
    NV_PLATFORM_PROCESS_NOTIFY_ROUTINE currentNotifyRoutine;
    void* currentContext;

    return NvPlatformThreadSetProcessNotifyRoutine(
        (NV_PLATFORM_PROCESS_NOTIFY_ROUTINE)previousNotifyRoutine,
        previousContext,
        &currentNotifyRoutine,
        &currentContext);
}
