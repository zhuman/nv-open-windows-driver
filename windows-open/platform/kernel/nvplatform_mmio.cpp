#include "nvplatform_mmio.h"

namespace {

bool IsValidRegisterAccess(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset) noexcept
{
    return mapping != nullptr &&
        mapping->address != nullptr &&
        (offset & (sizeof(ULONG) - 1)) == 0 &&
        offset <= mapping->length &&
        mapping->length - offset >= sizeof(ULONG);
}

} // namespace

extern "C" NTSTATUS NvPlatformMapMmio(
    _In_ PHYSICAL_ADDRESS physicalAddress,
    _In_ SIZE_T length,
    _Out_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept
{
    if (mapping == nullptr || length == 0 || physicalAddress.QuadPart < 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(mapping, sizeof(*mapping));

    constexpr ULONGLONG PageMask = PAGE_SIZE - 1;
    const SIZE_T pageOffset = static_cast<SIZE_T>(
        static_cast<ULONGLONG>(physicalAddress.QuadPart) & PageMask);
    if (length > MAXSIZE_T - pageOffset)
    {
        return STATUS_INTEGER_OVERFLOW;
    }

    const SIZE_T unroundedLength = length + pageOffset;
    if (unroundedLength > MAXSIZE_T - PageMask)
    {
        return STATUS_INTEGER_OVERFLOW;
    }

    const SIZE_T mappingLength = (unroundedLength + PageMask) & ~PageMask;
    PHYSICAL_ADDRESS alignedAddress = {};
    alignedAddress.QuadPart = physicalAddress.QuadPart & ~PageMask;

    void* mappingBase = MmMapIoSpaceEx(
        alignedAddress,
        mappingLength,
        PAGE_READWRITE | PAGE_NOCACHE);
    if (mappingBase == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mapping->mappingBase = mappingBase;
    mapping->address = static_cast<volatile UCHAR*>(mappingBase) + pageOffset;
    mapping->mappingLength = mappingLength;
    mapping->length = length;
    return STATUS_SUCCESS;
}

extern "C" void NvPlatformUnmapMmio(
    _Inout_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept
{
    if (mapping == nullptr)
    {
        return;
    }

    if (mapping->mappingBase != nullptr)
    {
        MmUnmapIoSpace(mapping->mappingBase, mapping->mappingLength);
    }

    RtlZeroMemory(mapping, sizeof(*mapping));
}

extern "C" NTSTATUS NvPlatformReadMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _Out_ ULONG* value) noexcept
{
    if (value == nullptr || !IsValidRegisterAccess(mapping, offset))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *value = READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG*>(
        mapping->address + offset));
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS NvPlatformWriteMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _In_ ULONG value) noexcept
{
    if (!IsValidRegisterAccess(mapping, offset))
    {
        return STATUS_INVALID_PARAMETER;
    }

    WRITE_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG*>(mapping->address + offset),
        value);
    return STATUS_SUCCESS;
}