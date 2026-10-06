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

typedef enum NV_PLATFORM_RM_MEMORY_PURPOSE
{
    NV_PLATFORM_RM_MEMORY_PURPOSE_DEFAULT,
    NV_PLATFORM_RM_MEMORY_PURPOSE_CACHED_PIXEL_STAGING
} NV_PLATFORM_RM_MEMORY_PURPOSE;

// Carry pixel-allocation provenance through RM without changing its wire ABI.
#define NV_PLATFORM_RM_CACHED_PIXEL_STAGING_TAG 0x5049584cU
// Bit 42 is reserved/unused in RM's memory-descriptor flags.
#define NV_PLATFORM_RM_MEMDESC_CACHED_PIXEL_STAGING NVBIT64(42)

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

NV_STATUS NvPlatformRmAllocatePagesWithPurpose(
    NvU64 size,
    NvU64 alignment,
    NvBool contiguous,
    NvBool pageable,
    NvBool belowFourGigabytes,
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType,
    NV_PLATFORM_RM_MEMORY_PURPOSE purpose,
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

// FBSR uses a delete-on-close file section so its maximum framebuffer-sized
// extent does not consume equivalent pagefile commit. Views live in system
// space because suspend callbacks are not tied to a stable process context.
NV_STATUS NvPlatformRmCreateSection(
    NvU64 maximumSize,
    void** section);

void NvPlatformRmDestroySection(void* section);

NV_STATUS NvPlatformRmMapSectionView(
    void* section,
    NvU64 offset,
    NvU64 length,
    void** address);

NV_STATUS NvPlatformRmUnmapSectionView(void* address);

// Pins a page-aligned system-space view in bounded MDL chunks and returns its
// PFNs to RM. The opaque range must be unlocked before the view is unmapped.
NV_STATUS NvPlatformRmLockSystemAddress(
    void* address,
    NvU64 length,
    void** lockedRange,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity,
    NvU32* physicalAddressCount);

void NvPlatformRmUnlockSystemAddress(void* lockedRange);

#ifdef __cplusplus
}
#endif
