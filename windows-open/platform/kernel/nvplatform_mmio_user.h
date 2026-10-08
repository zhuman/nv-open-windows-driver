#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NV_PLATFORM_MMIO_ACCESS
{
    void* context;
    UCHAR (*read8)(void* context, SIZE_T offset);
    USHORT (*read16)(void* context, SIZE_T offset);
    ULONG (*read32)(void* context, SIZE_T offset);
    void (*read32Buffer)(
        void* context,
        SIZE_T offset,
        ULONG* buffer,
        ULONG count);
    void (*write8)(void* context, SIZE_T offset, UCHAR value);
    void (*write16)(void* context, SIZE_T offset, USHORT value);
    void (*write32)(void* context, SIZE_T offset, ULONG value);
} NV_PLATFORM_MMIO_ACCESS;

typedef enum NV_PLATFORM_MMIO_CACHE_TYPE
{
    NV_PLATFORM_MMIO_CACHE_UNCACHED,
    NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
} NV_PLATFORM_MMIO_CACHE_TYPE;

#define NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH 0xFFFFF000ULL

typedef struct NV_PLATFORM_MMIO_MAPPING_PROVIDER
{
    void* context;
    NTSTATUS (*map)(
        void* context,
        ULONGLONG physicalAddress,
        SIZE_T length,
        NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
        volatile void** address,
        NV_PLATFORM_MMIO_ACCESS* access);
    // The provider owns access.context until unmap, including incomplete maps.
    void (*unmap)(void* context, void* mappingContext);
} NV_PLATFORM_MMIO_MAPPING_PROVIDER;

NTSTATUS NvPlatformRegisterMmioAperture(
    ULONGLONG physicalAddress,
    volatile void* address,
    SIZE_T length,
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
    const NV_PLATFORM_MMIO_ACCESS* access,
    void** registration) noexcept;

void NvPlatformUnregisterMmioAperture(void* registration) noexcept;

// Demand resources require a page-aligned base and share one bounded WDF window.
NTSTATUS NvPlatformRegisterMmioResource(
    ULONGLONG physicalAddress,
    ULONGLONG length,
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
    const NV_PLATFORM_MMIO_MAPPING_PROVIDER* provider,
    void** registration) noexcept;

void NvPlatformUnregisterMmioResource(void* registration) noexcept;

#ifdef __cplusplus
}

namespace nv::platform {

inline bool IsSupportedUserMmioRange(
    ULONGLONG physicalAddress, SIZE_T length, ULONG pageSize) noexcept
{
    if (length == 0 || length > NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH ||
        pageSize == 0 || (pageSize & (pageSize - 1)) != 0)
    {
        return false;
    }
    // Include the backing mapping's page padding in the sub-4-GiB limit.
    const ULONGLONG mask = static_cast<ULONGLONG>(pageSize) - 1;
    const ULONGLONG mappedLength =
        (length + (physicalAddress & mask) + mask) & ~mask;
    return mappedLength <= NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH;
}

} // namespace nv::platform
#endif