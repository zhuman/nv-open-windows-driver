#include "nvport/nvport.h"

#include "nvplatform_sync.h"

NvBool portUtilIsInterruptContext(void)
{
    return NvPlatformGetCurrentExecutionLevel() > 2 ? NV_TRUE : NV_FALSE;
}

void portUtilExSpinNs(NvU32 nanoseconds)
{
    const NvU64 startTime = portTimeGetUptimeNanosecondsHighPrecision();

    while ((portTimeGetUptimeNanosecondsHighPrecision() - startTime) < nanoseconds)
    {
        portUtilSpin();
    }
}

void portUtilExDelayMs(NvU32 milliseconds)
{
    if (milliseconds == 0)
    {
        return;
    }

    if (portSyncExSafeToSleep())
    {
        NvPlatformDelayMilliseconds(milliseconds);
        return;
    }

    while (milliseconds-- != 0)
    {
        portUtilExSpinNs(1000000U);
    }
}