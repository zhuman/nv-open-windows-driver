#include "hardware_resources.hpp"

namespace {

const CM_FULL_RESOURCE_DESCRIPTOR* NextFullDescriptor(
    _In_ const CM_FULL_RESOURCE_DESCRIPTOR& descriptor) noexcept
{
    const auto* next = descriptor.PartialResourceList.PartialDescriptors +
        descriptor.PartialResourceList.Count;
    return reinterpret_cast<const CM_FULL_RESOURCE_DESCRIPTOR*>(next);
}

} // namespace

namespace nv::kmdod {

HardwareResources::HardwareResources() noexcept
    : memoryRanges_{},
      memoryRangeCount_(0)
{
}

NTSTATUS HardwareResources::Initialize(
    _In_opt_ const CM_RESOURCE_LIST* translatedResources) noexcept
{
    Reset();

    if (translatedResources == nullptr)
    {
        return STATUS_SUCCESS;
    }

    const CM_FULL_RESOURCE_DESCRIPTOR* fullDescriptor = translatedResources->List;
    for (ULONG fullIndex = 0; fullIndex < translatedResources->Count; ++fullIndex)
    {
        const CM_PARTIAL_RESOURCE_LIST& partialList =
            fullDescriptor->PartialResourceList;

        for (ULONG partialIndex = 0; partialIndex < partialList.Count; ++partialIndex)
        {
            const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor =
                partialList.PartialDescriptors[partialIndex];

            if (descriptor.Type == CmResourceTypeMemory ||
                descriptor.Type == CmResourceTypeMemoryLarge)
            {
                const NTSTATUS status = AddMemoryRange(descriptor);
                if (!NT_SUCCESS(status))
                {
                    Reset();
                    return status;
                }
            }
        }

        fullDescriptor = NextFullDescriptor(*fullDescriptor);
    }

    return STATUS_SUCCESS;
}

void HardwareResources::Reset() noexcept
{
    RtlZeroMemory(memoryRanges_, sizeof(memoryRanges_));
    memoryRangeCount_ = 0;
}

ULONG HardwareResources::MemoryRangeCount() const noexcept
{
    return memoryRangeCount_;
}

const MemoryRange* HardwareResources::MemoryRangeAt(_In_ ULONG index) const noexcept
{
    return index < memoryRangeCount_ ? &memoryRanges_[index] : nullptr;
}

NTSTATUS HardwareResources::AddMemoryRange(
    _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor) noexcept
{
    if (memoryRangeCount_ == MaximumMemoryRangeCount)
    {
        return STATUS_BUFFER_OVERFLOW;
    }

    MemoryRange& range = memoryRanges_[memoryRangeCount_];
    const NTSTATUS status = DecodeMemoryRange(
        descriptor,
        &range.physicalAddress,
        &range.length);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    range.flags = descriptor.Flags;
    ++memoryRangeCount_;
    return STATUS_SUCCESS;
}

NTSTATUS HardwareResources::DecodeMemoryRange(
    _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor,
    _Out_ PHYSICAL_ADDRESS* physicalAddress,
    _Out_ SIZE_T* length) noexcept
{
    ULONGLONG decodedLength = 0;

    if (descriptor.Type == CmResourceTypeMemory)
    {
        *physicalAddress = descriptor.u.Memory.Start;
        decodedLength = descriptor.u.Memory.Length;
    }
    else if (descriptor.Type == CmResourceTypeMemoryLarge)
    {
        const USHORT largeMemoryType = descriptor.Flags & CM_RESOURCE_MEMORY_LARGE;
        switch (largeMemoryType)
        {
        case CM_RESOURCE_MEMORY_LARGE_40:
            *physicalAddress = descriptor.u.Memory40.Start;
            decodedLength = static_cast<ULONGLONG>(descriptor.u.Memory40.Length40) << 8;
            break;
        case CM_RESOURCE_MEMORY_LARGE_48:
            *physicalAddress = descriptor.u.Memory48.Start;
            decodedLength = static_cast<ULONGLONG>(descriptor.u.Memory48.Length48) << 16;
            break;
        case CM_RESOURCE_MEMORY_LARGE_64:
            *physicalAddress = descriptor.u.Memory64.Start;
            decodedLength = static_cast<ULONGLONG>(descriptor.u.Memory64.Length64) << 32;
            break;
        default:
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (decodedLength == 0 || decodedLength > MAXSIZE_T)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    *length = static_cast<SIZE_T>(decodedLength);
    return STATUS_SUCCESS;
}

} // namespace nv::kmdod