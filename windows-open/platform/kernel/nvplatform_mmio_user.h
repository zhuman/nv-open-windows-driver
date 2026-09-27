#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS NvPlatformRegisterMmioAperture(
    ULONGLONG physicalAddress,
    volatile void* address,
    SIZE_T length,
    void** registration) noexcept;

void NvPlatformUnregisterMmioAperture(void* registration) noexcept;

#ifdef __cplusplus
}
#endif