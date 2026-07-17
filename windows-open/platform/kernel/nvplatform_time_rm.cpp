#include "nvplatform_time_rm.h"

extern "C" {
#include <ntddk.h>
}

namespace {

constexpr NvU64 WindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr NvU64 TicksPerSecond100ns = 10000000ULL;
constexpr NvU64 TicksPerMicrosecond100ns = 10ULL;

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