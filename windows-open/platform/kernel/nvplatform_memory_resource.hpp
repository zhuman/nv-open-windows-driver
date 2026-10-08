#pragma once

namespace nv::platform {

struct MemoryResource
{
    PHYSICAL_ADDRESS physicalAddress;
    ULONGLONG length;
    USHORT flags;
};

inline bool IsMemoryResource(const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor) noexcept
{
    return descriptor.Type == CmResourceTypeMemory ||
        descriptor.Type == CmResourceTypeMemoryLarge;
}

inline NTSTATUS DecodeMemoryResource(
    const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor,
    MemoryResource* resource) noexcept
{
    if (resource == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *resource = {};
    MemoryResource decoded = {};
    if (descriptor.Type == CmResourceTypeMemory)
    {
        decoded.physicalAddress = descriptor.u.Memory.Start;
        decoded.length = descriptor.u.Memory.Length;
    }
    else if (descriptor.Type == CmResourceTypeMemoryLarge)
    {
        switch (descriptor.Flags & CM_RESOURCE_MEMORY_LARGE)
        {
        case CM_RESOURCE_MEMORY_LARGE_40:
            decoded.physicalAddress = descriptor.u.Memory40.Start;
            decoded.length = static_cast<ULONGLONG>(descriptor.u.Memory40.Length40) << 8;
            break;
        case CM_RESOURCE_MEMORY_LARGE_48:
            decoded.physicalAddress = descriptor.u.Memory48.Start;
            decoded.length = static_cast<ULONGLONG>(descriptor.u.Memory48.Length48) << 16;
            break;
        case CM_RESOURCE_MEMORY_LARGE_64:
            decoded.physicalAddress = descriptor.u.Memory64.Start;
            decoded.length = static_cast<ULONGLONG>(descriptor.u.Memory64.Length64) << 32;
            break;
        default:
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (decoded.length == 0 ||
        decoded.length - 1 > ~ULONGLONG{0} -
            static_cast<ULONGLONG>(decoded.physicalAddress.QuadPart))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    decoded.flags = descriptor.Flags;
    *resource = decoded;
    return STATUS_SUCCESS;
}

template<typename RawDescriptor, typename TranslatedDescriptor>
NTSTATUS ResolveMemoryBar(
    ULONGLONG pciAddress,
    ULONG rawCount,
    ULONG translatedCount,
    RawDescriptor rawDescriptor,
    TranslatedDescriptor translatedDescriptor,
    MemoryResource* resource) noexcept
{
    if (resource == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *resource = {};
    if (pciAddress == 0 || rawCount != translatedCount)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    bool found = false;
    MemoryResource resolved = {};
    for (ULONG index = 0; index < rawCount; ++index)
    {
        const auto* raw = rawDescriptor(index);
        const auto* translated = translatedDescriptor(index);
        if (raw == nullptr || translated == nullptr)
        {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (!IsMemoryResource(*raw))
        {
            continue;
        }
        MemoryResource rawRange = {};
        NTSTATUS status = DecodeMemoryResource(*raw, &rawRange);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (static_cast<ULONGLONG>(rawRange.physicalAddress.QuadPart) != pciAddress)
        {
            continue;
        }
        MemoryResource translatedRange = {};
        status = DecodeMemoryResource(*translated, &translatedRange);
        if (!NT_SUCCESS(status) || rawRange.length != translatedRange.length || found)
        {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        resolved = translatedRange;
        found = true;
    }
    if (!found)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    *resource = resolved;
    return STATUS_SUCCESS;
}

} // namespace nv::platform
