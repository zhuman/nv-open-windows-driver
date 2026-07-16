#pragma once

extern "C" {
#include <ntddk.h>
}

struct NV_PLATFORM_MMIO_MAPPING
{
    void* mappingBase;
    volatile UCHAR* address;
    SIZE_T mappingLength;
    SIZE_T length;
};

extern "C" NTSTATUS NvPlatformMapMmio(
    _In_ PHYSICAL_ADDRESS physicalAddress,
    _In_ SIZE_T length,
    _Out_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept;

extern "C" void NvPlatformUnmapMmio(
    _Inout_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept;

extern "C" NTSTATUS NvPlatformReadMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _Out_ ULONG* value) noexcept;

extern "C" NTSTATUS NvPlatformWriteMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _In_ ULONG value) noexcept;