#pragma once

#include <ntddk.h>

#ifdef __cplusplus
extern "C" {
#define NV_PLATFORM_NOEXCEPT noexcept
#else
#define NV_PLATFORM_NOEXCEPT
#endif

NTSTATUS NvPlatformRegistryInitialize(
    _In_ const UNICODE_STRING* registryPath) NV_PLATFORM_NOEXCEPT;

void NvPlatformRegistryShutdown(void) NV_PLATFORM_NOEXCEPT;

NTSTATUS NvPlatformReadRegistryDword(
    _In_opt_ const wchar_t* relativeSubkey,
    _In_ const wchar_t* valueName,
    _Out_ ULONG* value) NV_PLATFORM_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef NV_PLATFORM_NOEXCEPT