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
    Log,
};

} // namespace

extern "C" const NV_COMMON_HOST_SERVICES* NvPlatformGetCommonHostServices(void)
{
    return &Services;
}