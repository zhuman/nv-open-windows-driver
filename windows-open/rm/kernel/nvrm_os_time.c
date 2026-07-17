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

NvU64 osGetMonotonicTimeNs(void)
{
    return NvPlatformGetRmMonotonicTimeNs();
}

NV_STATUS osGetPerformanceCounter(NvU64* timeNs)
{
    return NvPlatformGetRmPerformanceCounter(timeNs);
}

NV_STATUS osDelay(NvU32 milliseconds)
{
    return NvPlatformDelayRmMilliseconds(milliseconds);
}

NV_STATUS osDelayUs(NvU32 microseconds)
{
    return NvPlatformDelayRmMicroseconds(microseconds);
}

NV_STATUS osDelayNs(NvU32 nanoseconds)
{
    NvU32 microseconds = nanoseconds / 1000;
    if (microseconds == 0)
    {
        microseconds = 1;
    }

    return NvPlatformDelayRmMicroseconds(microseconds);
}