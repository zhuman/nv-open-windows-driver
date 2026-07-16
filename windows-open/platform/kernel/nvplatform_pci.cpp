#include "nvplatform_pci.h"

namespace {

constexpr ULONG PciExpressConfigurationLength = 4096;

bool IsValidConfigurationRange(_In_ ULONG offset, _In_ ULONG length) noexcept
{
    return length != 0 &&
        length <= PciExpressConfigurationLength &&
        offset <= PciExpressConfigurationLength - length;
}

} // namespace

extern "C" NTSTATUS NvPlatformReadPciConfig(
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _In_ ULONG offset,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG length) noexcept
{
    if (access == nullptr || buffer == nullptr ||
        !IsValidConfigurationRange(offset, length))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (access->read == nullptr)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ULONG bytesRead = 0;
    const NTSTATUS status = access->read(
        access->context,
        buffer,
        offset,
        length,
        &bytesRead);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return bytesRead == length
        ? STATUS_SUCCESS
        : STATUS_DEVICE_CONFIGURATION_ERROR;
}

extern "C" NTSTATUS NvPlatformWritePciConfig(
    _In_ const NV_PLATFORM_PCI_CONFIG_ACCESS* access,
    _In_ ULONG offset,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG length) noexcept
{
    if (access == nullptr || buffer == nullptr ||
        !IsValidConfigurationRange(offset, length))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (access->write == nullptr)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ULONG bytesWritten = 0;
    const NTSTATUS status = access->write(
        access->context,
        buffer,
        offset,
        length,
        &bytesWritten);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return bytesWritten == length
        ? STATUS_SUCCESS
        : STATUS_DEVICE_CONFIGURATION_ERROR;
}