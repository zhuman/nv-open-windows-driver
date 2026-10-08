#include "nvplatform_resources.h"
#include "nvplatform_memory_resource.hpp"

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
    nv::platform::MemoryResource decoded = {};
    const NTSTATUS status = nv::platform::DecodeMemoryResource(descriptor, &decoded);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    if (decoded.length > MAXSIZE_T)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    resource->physicalAddress = decoded.physicalAddress;
    resource->length = static_cast<SIZE_T>(decoded.length);
    resource->flags = decoded.flags;
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