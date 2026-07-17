#include "nvplatform_cpu_rm.h"
#include "nvplatform_thread.h"

#include <stddef.h>

typedef NvU64 OS_THREAD_HANDLE;

NvU32 osGetMaximumCoreCount(void)
{
    return NvPlatformGetRmCpuCount();
}

NV_STATUS osGetCurrentThread(OS_THREAD_HANDLE* threadId)
{
    if (threadId == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *threadId = NvPlatformGetCurrentThreadId();
    return NV_OK;
}

NvU32 osGetCurrentProcessFlags(void)
{
    return NvPlatformGetCurrentProcessFlags();
}

NV_STATUS osSchedule(void)
{
    return NvPlatformScheduleThread();
}