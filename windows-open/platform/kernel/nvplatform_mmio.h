#pragma once

#include <ntddk.h>

#ifdef __cplusplus
extern "C" {
#define NV_PLATFORM_NOEXCEPT noexcept
#else
#define NV_PLATFORM_NOEXCEPT
#endif

typedef struct NV_PLATFORM_MMIO_MAPPING
{
    void* mappingBase;
    volatile UCHAR* address;
    SIZE_T mappingLength;
    SIZE_T length;
} NV_PLATFORM_MMIO_MAPPING;

NTSTATUS NvPlatformMapMmio(
    _In_ PHYSICAL_ADDRESS physicalAddress,
    _In_ SIZE_T length,
    _Out_ NV_PLATFORM_MMIO_MAPPING* mapping) NV_PLATFORM_NOEXCEPT;

void NvPlatformUnmapMmio(
    _Inout_ NV_PLATFORM_MMIO_MAPPING* mapping) NV_PLATFORM_NOEXCEPT;

NTSTATUS NvPlatformReadMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _Out_ ULONG* value) NV_PLATFORM_NOEXCEPT;

NTSTATUS NvPlatformWriteMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _In_ ULONG value) NV_PLATFORM_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef NV_PLATFORM_NOEXCEPT