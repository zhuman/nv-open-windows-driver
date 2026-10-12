#include "common_services.hpp"
#include "nvplatform_common.h"

#include <windows.h>

namespace {

void* Allocate(void* providerContext, NvU64 length, NvBool nonPaged)
{
    UNREFERENCED_PARAMETER(providerContext);
    UNREFERENCED_PARAMETER(nonPaged);

    if (length == 0 || length > MAXSIZE_T)
    {
        return nullptr;
    }

    return HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(length));
}

void Free(void* providerContext, void* allocation, NvBool nonPaged)
{
    UNREFERENCED_PARAMETER(providerContext);
    UNREFERENCED_PARAMETER(nonPaged);
    HeapFree(GetProcessHeap(), 0, allocation);
}

NvU64 GetMonotonicTime100ns(void* providerContext)
{
    UNREFERENCED_PARAMETER(providerContext);
    return GetTickCount64() * 10000ULL;
}

NvU64 GetUnixTime100ns(void* providerContext)
{
    constexpr NvU64 WindowsToUnixEpoch100ns = 116444736000000000ULL;
    FILETIME fileTime{};
    ULARGE_INTEGER systemTime{};

    UNREFERENCED_PARAMETER(providerContext);
    GetSystemTimePreciseAsFileTime(&fileTime);
    systemTime.LowPart = fileTime.dwLowDateTime;
    systemTime.HighPart = fileTime.dwHighDateTime;
    return systemTime.QuadPart - WindowsToUnixEpoch100ns;
}

NvBool IsAllocationSafe(void* providerContext, NvBool nonPaged)
{
    UNREFERENCED_PARAMETER(providerContext);
    UNREFERENCED_PARAMETER(nonPaged);
    return NV_TRUE;
}

void Log(void* providerContext, NvU32 level, const char* message)
{
    UNREFERENCED_PARAMETER(providerContext);
    NvPlatformDebugPrintf(level, "%s", message);
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

namespace nv::iddcx {

const NV_COMMON_HOST_SERVICES* GetCommonHostServices() noexcept
{
    return &Services;
}

} // namespace nv::iddcx