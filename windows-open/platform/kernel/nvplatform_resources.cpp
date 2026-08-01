#include "nvplatform_resources.h"

namespace {

const CM_FULL_RESOURCE_DESCRIPTOR* NextFullDescriptor(
    _In_ const CM_FULL_RESOURCE_DESCRIPTOR& descriptor) noexcept
{
    const auto* next = descriptor.PartialResourceList.PartialDescriptors +
        descriptor.PartialResourceList.Count;
    return reinterpret_cast<const CM_FULL_RESOURCE_DESCRIPTOR*>(next);
}

NTSTATUS DecodeMemoryResource(
    _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor,
    _Out_ NV_PLATFORM_MEMORY_RESOURCE* resource) noexcept
{
    ULONGLONG decodedLength = 0;

    if (descriptor.Type == CmResourceTypeMemory)
    {
        resource->physicalAddress = descriptor.u.Memory.Start;
        decodedLength = descriptor.u.Memory.Length;
    }
    else if (descriptor.Type == CmResourceTypeMemoryLarge)
    {
        switch (descriptor.Flags & CM_RESOURCE_MEMORY_LARGE)
        {
        case CM_RESOURCE_MEMORY_LARGE_40:
            resource->physicalAddress = descriptor.u.Memory40.Start;
            decodedLength = static_cast<ULONGLONG>(descriptor.u.Memory40.Length40) << 8;
            break;
        case CM_RESOURCE_MEMORY_LARGE_48:
            resource->physicalAddress = descriptor.u.Memory48.Start;
            decodedLength = static_cast<ULONGLONG>(descriptor.u.Memory48.Length48) << 16;
            break;
        case CM_RESOURCE_MEMORY_LARGE_64:
            resource->physicalAddress = descriptor.u.Memory64.Start;
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

    resource->length = static_cast<SIZE_T>(decodedLength);
    resource->flags = descriptor.Flags;
    return STATUS_SUCCESS;
}

void DecodeInterruptResource(
    _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor,
    _Out_ NV_PLATFORM_INTERRUPT_RESOURCE* resource) noexcept
{
    resource->flags = descriptor.Flags;
    resource->messageSignaled =
        (descriptor.Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0;

    if (resource->messageSignaled)
    {
        resource->vector = descriptor.u.MessageInterrupt.Translated.Vector;
        resource->level = descriptor.u.MessageInterrupt.Translated.Level;
        resource->affinity = descriptor.u.MessageInterrupt.Translated.Affinity;
    }
    else
    {
        resource->vector = descriptor.u.Interrupt.Vector;
        resource->level = descriptor.u.Interrupt.Level;
        resource->affinity = descriptor.u.Interrupt.Affinity;
    }
}

} // namespace

extern "C" NTSTATUS NvPlatformDecodeTranslatedResources(
    const CM_RESOURCE_LIST* translatedResources,
    NV_PLATFORM_TRANSLATED_RESOURCES* resources) noexcept
{
    if (resources == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(resources, sizeof(*resources));
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

            NTSTATUS status = STATUS_SUCCESS;
            if (descriptor.Type == CmResourceTypeMemory ||
                descriptor.Type == CmResourceTypeMemoryLarge)
            {
                if (resources->memoryCount == NV_PLATFORM_MAX_MEMORY_RESOURCE_COUNT)
                {
                    status = STATUS_BUFFER_OVERFLOW;
                }
                else
                {
                    status = DecodeMemoryResource(
                        descriptor,
                        &resources->memory[resources->memoryCount]);
                    if (NT_SUCCESS(status))
                    {
                        ++resources->memoryCount;
                    }
                }
            }
            else if (descriptor.Type == CmResourceTypeInterrupt)
            {
                if (resources->interruptCount == NV_PLATFORM_MAX_INTERRUPT_RESOURCE_COUNT)
                {
                    status = STATUS_BUFFER_OVERFLOW;
                }
                else
                {
                    DecodeInterruptResource(
                        descriptor,
                        &resources->interrupts[resources->interruptCount]);
                    ++resources->interruptCount;
                }
            }

            if (!NT_SUCCESS(status))
            {
                RtlZeroMemory(resources, sizeof(*resources));
                return status;
            }
        }

        fullDescriptor = NextFullDescriptor(*fullDescriptor);
    }

    return STATUS_SUCCESS;
}