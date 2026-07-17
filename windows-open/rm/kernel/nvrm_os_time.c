#include "nvplatform_time_rm.h"

NV_STATUS osGetSystemTime(NvU32* seconds, NvU32* microseconds)
{
    return NvPlatformGetRmSystemTime(seconds, microseconds);
}

NvU64 osGetTimestamp(void)
{
    return NvPlatformGetRmTimestamp();
}

NvU64 osGetTimestampFreq(void)
{
    return NvPlatformGetRmTimestampFrequency();
}