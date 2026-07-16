#include "nvplatform_common.h"

extern "C" {
#include <ntddk.h>
}

namespace {

constexpr ULONG NvCommonPoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('C') << 16) |
    (static_cast<ULONG>('m') << 24);

void* Allocate(
    void* providerContext,
    NvU64 length,
    NvBool nonPaged)
{
    UNREFERENCED_PARAMETER(providerContext);

    if (length == 0 || length > MAXSIZE_T)
    {
        return nullptr;
    }

    const POOL_FLAGS poolFlags =
        (nonPaged ? POOL_FLAG_NON_PAGED : POOL_FLAG_PAGED);
    return ExAllocatePool2(poolFlags, static_cast<SIZE_T>(length), NvCommonPoolTag);
}

void Free(void* providerContext, void* allocation, NvBool nonPaged)
{
    UNREFERENCED_PARAMETER(providerContext);
    UNREFERENCED_PARAMETER(nonPaged);
    ExFreePoolWithTag(allocation, NvCommonPoolTag);
}

NvU64 GetMonotonicTime100ns(void* providerContext)
{
    UNREFERENCED_PARAMETER(providerContext);
    return KeQueryUnbiasedInterruptTime();
}

NvU64 GetUnixTime100ns(void* providerContext)
{
    constexpr NvU64 WindowsToUnixEpoch100ns = 116444736000000000ULL;
    LARGE_INTEGER systemTime{};

    UNREFERENCED_PARAMETER(providerContext);
    KeQuerySystemTimePrecise(&systemTime);
    return static_cast<NvU64>(systemTime.QuadPart) - WindowsToUnixEpoch100ns;
}

NvBool IsAllocationSafe(void* providerContext, NvBool nonPaged)
{
    UNREFERENCED_PARAMETER(providerContext);
    const KIRQL maximumIrql = nonPaged ? DISPATCH_LEVEL : APC_LEVEL;
    return KeGetCurrentIrql() <= maximumIrql ? NV_TRUE : NV_FALSE;
}

void Log(void* providerContext, NvU32 level, const char* message)
{
    UNREFERENCED_PARAMETER(providerContext);
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, level, "%s", message);
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