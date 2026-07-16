#pragma once

#include <ntddk.h>

#ifdef __cplusplus
extern "C" {
#define NV_PLATFORM_NOEXCEPT noexcept
#else
#define NV_PLATFORM_NOEXCEPT
#endif

typedef NTSTATUS (*NV_PLATFORM_PCI_CONFIG_READ)(
    _In_ void* context,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG offset,
    _In_ ULONG length,
    _Out_ ULONG* bytesRead);

typedef NTSTATUS (*NV_PLATFORM_PCI_CONFIG_WRITE)(
    _In_ void* context,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG offset,
    _In_ ULONG length,
    _Out_ ULONG* bytesWritten);

typedef struct NV_PLATFORM_PCI_CONFIG_ACCESS
{
    void* context;
    NV_PLATFORM_PCI_CONFIG_READ read;
    NV_PLATFORM_PCI_CONFIG_WRITE write;
} NV_PLATFORM_PCI_CONFIG_ACCESS;

NTSTATUS NvPlatformReadPciConfig(
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _In_ ULONG offset,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG length) NV_PLATFORM_NOEXCEPT;

NTSTATUS NvPlatformWritePciConfig(
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _In_ ULONG offset,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG length) NV_PLATFORM_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef NV_PLATFORM_NOEXCEPT