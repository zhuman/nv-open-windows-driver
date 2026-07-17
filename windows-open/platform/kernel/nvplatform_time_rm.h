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

#ifdef __cplusplus
}
#endif