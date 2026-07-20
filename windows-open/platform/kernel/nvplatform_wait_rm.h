#pragma once

#include "nvstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmWaitQueueCreate(void** waitQueue);
void NvPlatformRmWaitQueueDestroy(void* waitQueue);
void NvPlatformRmWaitQueueWait(void* waitQueue, NvBool alertable);
void NvPlatformRmWaitQueueWake(void* waitQueue);

#ifdef __cplusplus
}
#endif