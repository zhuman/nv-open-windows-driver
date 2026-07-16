#include "common_services.hpp"

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

void Log(void* providerContext, NvU32 level, const char* message)
{
    UNREFERENCED_PARAMETER(providerContext);
    UNREFERENCED_PARAMETER(level);
    OutputDebugStringA(message);
}

const NV_COMMON_HOST_SERVICES Services = {
    NV_COMMON_HOST_SERVICES_VERSION,
    sizeof(NV_COMMON_HOST_SERVICES),
    nullptr,
    Allocate,
    Free,
    GetMonotonicTime100ns,
    Log,
};

} // namespace

namespace nv::iddcx {

const NV_COMMON_HOST_SERVICES* GetCommonHostServices() noexcept
{
    return &Services;
}

} // namespace nv::iddcx