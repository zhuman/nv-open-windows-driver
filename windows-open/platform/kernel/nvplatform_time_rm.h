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

#ifdef __cplusplus
}
#endif