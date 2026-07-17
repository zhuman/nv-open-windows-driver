#include "nvplatform_time_rm.h"

extern "C" {
#include <ntddk.h>
}

namespace {

constexpr NvU64 WindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr NvU64 TicksPerSecond100ns = 10000000ULL;
constexpr NvU64 TicksPerMicrosecond100ns = 10ULL;
constexpr NvU32 MaximumInterruptDelayMicroseconds = 20000;
constexpr ULONG StallChunkMicroseconds = 50;

void StallMicroseconds(NvU64 microseconds)
{
    while (microseconds != 0)
    {
        const ULONG chunk = microseconds > StallChunkMicroseconds
            ? StallChunkMicroseconds
            : static_cast<ULONG>(microseconds);
        KeStallExecutionProcessor(chunk);
        microseconds -= chunk;
    }
}

NvU64 QueryPerformanceCounterNs()
{
    LARGE_INTEGER frequency{};
    const LARGE_INTEGER counter = KeQueryPerformanceCounter(&frequency);
    if (frequency.QuadPart <= 0 || counter.QuadPart < 0)
    {
        return 0;
    }

    constexpr NvU64 NanosecondsPerSecond = 1000000000ULL;
    const NvU64 ticks = static_cast<NvU64>(counter.QuadPart);
    const NvU64 ticksPerSecond = static_cast<NvU64>(frequency.QuadPart);
    return (ticks / ticksPerSecond) * NanosecondsPerSecond +
        ((ticks % ticksPerSecond) * NanosecondsPerSecond) / ticksPerSecond;
}

} // namespace

extern "C" NV_STATUS NvPlatformGetRmSystemTime(
    NvU32* seconds,
    NvU32* microseconds)
{
    if (seconds == nullptr || microseconds == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    LARGE_INTEGER systemTime{};
    KeQuerySystemTimePrecise(&systemTime);
    const NvU64 windowsTime = static_cast<NvU64>(systemTime.QuadPart);
    if (windowsTime < WindowsToUnixEpoch100ns)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    const NvU64 unixTime = windowsTime - WindowsToUnixEpoch100ns;
    *seconds = static_cast<NvU32>(unixTime / TicksPerSecond100ns);
    *microseconds = static_cast<NvU32>(
        (unixTime % TicksPerSecond100ns) / TicksPerMicrosecond100ns);
    return NV_OK;
}

extern "C" NvU64 NvPlatformGetRmTimestamp(void)
{
    return static_cast<NvU64>(KeQueryPerformanceCounter(nullptr).QuadPart);
}

extern "C" NvU64 NvPlatformGetRmTimestampFrequency(void)
{
    LARGE_INTEGER frequency{};
    KeQueryPerformanceCounter(&frequency);
    return static_cast<NvU64>(frequency.QuadPart);
}

extern "C" NvU64 NvPlatformGetRmMonotonicTimeNs(void)
{
    return KeQueryUnbiasedInterruptTime() * 100ULL;
}

extern "C" NV_STATUS NvPlatformGetRmPerformanceCounter(NvU64* timeNs)
{
    if (timeNs == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *timeNs = QueryPerformanceCounterNs();
    return *timeNs != 0 ? NV_OK : NV_ERR_OPERATING_SYSTEM;
}

extern "C" NV_STATUS NvPlatformDelayRmMilliseconds(NvU32 milliseconds)
{
    const KIRQL irql = KeGetCurrentIrql();
    if (irql > DISPATCH_LEVEL &&
        milliseconds > MaximumInterruptDelayMicroseconds / 1000)
    {
        return NV_ERR_GENERIC;
    }

    if (irql <= APC_LEVEL)
    {
        LARGE_INTEGER interval{};
        interval.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000LL;
        (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    else
    {
        StallMicroseconds(static_cast<NvU64>(milliseconds) * 1000ULL);
    }

    return NV_OK;
}

extern "C" NV_STATUS NvPlatformDelayRmMicroseconds(NvU32 microseconds)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL &&
        microseconds > MaximumInterruptDelayMicroseconds)
    {
        return NV_ERR_GENERIC;
    }

    StallMicroseconds(microseconds);
    return NV_OK;
}