#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformCopyFromUser(
    const void* userAddress,
    void* kernelAddress,
    NvLength length);

NV_STATUS NvPlatformCopyToUser(
    const void* kernelAddress,
    void* userAddress,
    NvLength length);

typedef enum NV_PLATFORM_RM_MEMORY_CACHE_TYPE
{
    NV_PLATFORM_RM_MEMORY_CACHED,
    NV_PLATFORM_RM_MEMORY_UNCACHED,
    NV_PLATFORM_RM_MEMORY_WRITE_COMBINED
} NV_PLATFORM_RM_MEMORY_CACHE_TYPE;

NV_STATUS NvPlatformRmAllocatePages(
    NvU64 size,
    NvU64 alignment,
    NvBool contiguous,
    NvBool pageable,
    NvBool belowFourGigabytes,
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType,
    void** allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity,
    NvU32* physicalAddressCount);

void NvPlatformRmFreePages(void* allocation);

NV_STATUS NvPlatformRmLockPages(
    void* allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity);

NV_STATUS NvPlatformRmUnlockPages(void* allocation);

NV_STATUS NvPlatformRmGetPageCount(
    const void* allocation,
    NvU32* pageCount);

NV_STATUS NvPlatformRmGetPageTokens(
    const void* allocation,
    NvU64* pageTokens,
    NvU32* pageCount);

NvU64 NvPlatformRmPageTokenToPhysicalAddress(
    const void* pageTokens,
    NvU32 pageIndex);

NV_STATUS NvPlatformRmMapPages(
    void* allocation,
    NvU64 offset,
    NvU64 length,
    NvBool kernelMapping,
    NvBool writable,
    void** address,
    void** mapping);

void NvPlatformRmUnmapPages(
    NvBool kernelMapping,
    void* mapping);

#ifdef __cplusplus
}
#endif
