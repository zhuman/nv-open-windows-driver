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

NTSTATUS NvPlatformRegisterMmioAperture(
    ULONGLONG physicalAddress,
    volatile void* address,
    SIZE_T length,
    const NV_PLATFORM_MMIO_ACCESS* access,
    void** registration) noexcept;

void NvPlatformUnregisterMmioAperture(void* registration) noexcept;

#ifdef __cplusplus
}
#endif