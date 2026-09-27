#include "nvplatform_time_rm.h"

#include <limits.h>
#include <windows.h>

namespace {

constexpr NvU64 WindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr NvU64 TicksPerSecond100ns = 10000000ULL;
constexpr NvU64 NanosecondsPerSecond = 1000000000ULL;

struct NanoTimer
{
    PTP_TIMER timer;
    NvPlatformRmNanoTimerFunction* function;
    void* gpu;
    void* timerEvent;
};

VOID CALLBACK NanoTimerCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER)
{
    NanoTimer* timer = static_cast<NanoTimer*>(context);
    timer->function(timer->gpu, timer->timerEvent);
}

NvU64 QueryPerformanceCounterNs()
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&counter) ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        return 0;
    }

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

    FILETIME fileTime{};
    ULARGE_INTEGER systemTime{};
    GetSystemTimePreciseAsFileTime(&fileTime);
    systemTime.LowPart = fileTime.dwLowDateTime;
    systemTime.HighPart = fileTime.dwHighDateTime;
    if (systemTime.QuadPart < WindowsToUnixEpoch100ns)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    const NvU64 unixTime = systemTime.QuadPart - WindowsToUnixEpoch100ns;
    *seconds = static_cast<NvU32>(unixTime / TicksPerSecond100ns);
    *microseconds = static_cast<NvU32>((unixTime % TicksPerSecond100ns) / 10ULL);
    return NV_OK;
}

extern "C" NvU64 NvPlatformGetRmTimestamp(void)
{
    LARGE_INTEGER counter{};
    return QueryPerformanceCounter(&counter) ?
        static_cast<NvU64>(counter.QuadPart) : 0;
}

extern "C" NvU64 NvPlatformGetRmTimestampFrequency(void)
{
    LARGE_INTEGER frequency{};
    return QueryPerformanceFrequency(&frequency) ?
        static_cast<NvU64>(frequency.QuadPart) : 0;
}

extern "C" NvU64 NvPlatformGetRmMonotonicTimeNs(void)
{
    return QueryPerformanceCounterNs();
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
    Sleep(milliseconds);
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformDelayRmMicroseconds(NvU32 microseconds)
{
    const NvU64 start = QueryPerformanceCounterNs();
    if (start == 0)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }

    const NvU64 duration = static_cast<NvU64>(microseconds) * 1000ULL;
    while (QueryPerformanceCounterNs() - start < duration)
    {
        YieldProcessor();
    }
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmNanoTimerCreate(
    NvPlatformRmNanoTimerFunction* function,
    void* gpu,
    void* timerEvent,
    void** timer)
{
    if (function == nullptr || gpu == nullptr || timerEvent == nullptr || timer == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NanoTimer* nanoTimer = static_cast<NanoTimer*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NanoTimer)));
    if (nanoTimer == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    nanoTimer->function = function;
    nanoTimer->gpu = gpu;
    nanoTimer->timerEvent = timerEvent;
    nanoTimer->timer = CreateThreadpoolTimer(NanoTimerCallback, nanoTimer, nullptr);
    if (nanoTimer->timer == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, nanoTimer);
        return NV_ERR_OPERATING_SYSTEM;
    }

    *timer = nanoTimer;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmNanoTimerStart(void* timer, NvU64 relativeTimeNs)
{
    if (timer == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NvU64 ticks = (relativeTimeNs + 99ULL) / 100ULL;
    if (ticks == 0)
    {
        ticks = 1;
    }
    if (ticks > static_cast<NvU64>(LLONG_MAX))
    {
        ticks = static_cast<NvU64>(LLONG_MAX);
    }

    ULARGE_INTEGER dueTime{};
    dueTime.QuadPart = static_cast<NvU64>(-static_cast<LONGLONG>(ticks));
    FILETIME fileTime{};
    fileTime.dwLowDateTime = dueTime.LowPart;
    fileTime.dwHighDateTime = dueTime.HighPart;
    SetThreadpoolTimer(static_cast<NanoTimer*>(timer)->timer, &fileTime, 0, 0);
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmNanoTimerCancel(void* timer)
{
    if (timer == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NanoTimer* nanoTimer = static_cast<NanoTimer*>(timer);
    SetThreadpoolTimer(nanoTimer->timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(nanoTimer->timer, TRUE);
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmNanoTimerDestroy(void* timer)
{
    const NV_STATUS status = NvPlatformRmNanoTimerCancel(timer);
    if (status != NV_OK)
    {
        return status;
    }

    NanoTimer* nanoTimer = static_cast<NanoTimer*>(timer);
    CloseThreadpoolTimer(nanoTimer->timer);
    HeapFree(GetProcessHeap(), 0, nanoTimer);
    return NV_OK;
}