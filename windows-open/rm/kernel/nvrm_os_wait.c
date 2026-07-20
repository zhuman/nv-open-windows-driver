#include "g_os_nvoc.h"
#include "nvplatform_wait_rm.h"

NV_STATUS osAllocWaitQueue(OS_WAIT_QUEUE** waitQueue)
{
    return NvPlatformRmWaitQueueCreate((void**)waitQueue);
}

void osFreeWaitQueue(OS_WAIT_QUEUE* waitQueue)
{
    NvPlatformRmWaitQueueDestroy(waitQueue);
}

void osWaitUninterruptible(OS_WAIT_QUEUE* waitQueue)
{
    NvPlatformRmWaitQueueWait(waitQueue, NV_FALSE);
}

void osWaitInterruptible(OS_WAIT_QUEUE* waitQueue)
{
    NvPlatformRmWaitQueueWait(waitQueue, NV_TRUE);
}

void osWakeUp(OS_WAIT_QUEUE* waitQueue)
{
    NvPlatformRmWaitQueueWake(waitQueue);
}