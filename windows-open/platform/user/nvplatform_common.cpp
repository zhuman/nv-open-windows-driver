#include "nvplatform_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include <atomic>

namespace {

std::atomic<NV_PLATFORM_DEBUG_LOG> DebugLog{nullptr};

void* Allocate(void*, NvU64 length, NvBool)
{
    if (length == 0 || length > SIZE_MAX)
    {
        return nullptr;
    }

    return HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(length));
}

void Free(void*, void* allocation, NvBool)
{
    if (allocation != nullptr)
    {
        HeapFree(GetProcessHeap(), 0, allocation);
    }
}

NvU64 GetMonotonicTime100ns(void*)
{
    return GetTickCount64() * 10000ULL;
}

NvU64 GetUnixTime100ns(void*)
{
    constexpr NvU64 WindowsToUnixEpoch100ns = 116444736000000000ULL;
    FILETIME fileTime{};
    ULARGE_INTEGER systemTime{};

    GetSystemTimePreciseAsFileTime(&fileTime);
    systemTime.LowPart = fileTime.dwLowDateTime;
    systemTime.HighPart = fileTime.dwHighDateTime;
    return systemTime.QuadPart - WindowsToUnixEpoch100ns;
}

NvBool IsAllocationSafe(void*, NvBool)
{
    return NV_TRUE;
}

void Log(void*, NvU32 level, const char* message)
{
    if (message != nullptr)
    {
        OutputDebugStringA(message);
        if (const auto log = DebugLog.load(std::memory_order_acquire))
        {
            log(level, message);
        }
    }
}

const NV_COMMON_HOST_SERVICES Services = {
    NV_COMMON_HOST_SERVICES_VERSION,
    sizeof(NV_COMMON_HOST_SERVICES),
    nullptr,
    Allocate,
    Free,
    GetMonotonicTime100ns,
    GetUnixTime100ns,
    IsAllocationSafe,
    Log,
};

} // namespace

extern "C" const NV_COMMON_HOST_SERVICES* NvPlatformGetCommonHostServices(void)
{
    return &Services;
}

extern "C" NvU64 NvPlatformGetPageSize(void)
{
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    return systemInfo.dwPageSize;
}

extern "C" void NvPlatformSetDebugLog(NV_PLATFORM_DEBUG_LOG log)
{
    DebugLog.store(log, std::memory_order_release);
}

extern "C" void NvPlatformDebugPrintf(
    NvU32 level,
    const char* format,
    ...)
{
    char buffer[1024];
    va_list arguments;

    if (format == nullptr)
    {
        return;
    }

    va_start(arguments, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, arguments);
    va_end(arguments);
    Log(nullptr, level, buffer);
}