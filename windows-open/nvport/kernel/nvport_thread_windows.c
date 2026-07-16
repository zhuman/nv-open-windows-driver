#include "nvport/nvport.h"
#include "nvport/thread.h"

#include "nvplatform_thread.h"

const PORT_THREAD PORT_THREAD_INVALID = {0ULL};
const PORT_PROCESS PORT_PROCESS_INVALID = {0ULL};

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
