#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformGetRmSystemTime(
    NvU32* seconds,
    NvU32* microseconds);

NvU64 NvPlatformGetRmTimestamp(void);

NvU64 NvPlatformGetRmTimestampFrequency(void);

NvU64 NvPlatformGetRmMonotonicTimeNs(void);

NV_STATUS NvPlatformGetRmPerformanceCounter(NvU64* timeNs);

NV_STATUS NvPlatformDelayRmMilliseconds(NvU32 milliseconds);

NV_STATUS NvPlatformDelayRmMicroseconds(NvU32 microseconds);

typedef void NvPlatformRmNanoTimerFunction(
    void* gpu,
    void* timerEvent);

// The WDK provider fires at DPC level; the callback bridge must defer RM timer
// service to a passive-level GPU work item. Cancel and destroy are synchronous.
NV_STATUS NvPlatformRmNanoTimerCreate(
    NvPlatformRmNanoTimerFunction* function,
    void* gpu,
    void* timerEvent,
    void** timer);
NV_STATUS NvPlatformRmNanoTimerStart(void* timer, NvU64 relativeTimeNs);
NV_STATUS NvPlatformRmNanoTimerCancel(void* timer);
NV_STATUS NvPlatformRmNanoTimerDestroy(void* timer);

#ifdef __cplusplus
}
#endif