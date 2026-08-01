#include "nvplatform_cpu_rm.h"
#include "nvplatform_thread.h"

#include <stddef.h>

typedef NvU64 OS_THREAD_HANDLE;

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

NvU32 osGetCurrentProcess(void)
{
    return (NvU32)NvPlatformGetCurrentProcessId();
}

void osGetCurrentProcessName(char* processName, NvU32 length)
{
    NvPlatformGetCurrentProcessName(processName, length);
}

NV_STATUS osSchedule(void)
{
    return NvPlatformScheduleThread();
}