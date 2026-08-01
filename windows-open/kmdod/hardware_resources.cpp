#include "hardware_resources.hpp"

namespace nv::kmdod {

HardwareResources::HardwareResources() noexcept
    : resources_{}
{
}

NTSTATUS HardwareResources::Initialize(
    _In_opt_ const CM_RESOURCE_LIST* translatedResources) noexcept
{
    return NvPlatformDecodeTranslatedResources(translatedResources, &resources_);
}

void HardwareResources::Reset() noexcept
{
    RtlZeroMemory(&resources_, sizeof(resources_));
}

ULONG HardwareResources::MemoryRangeCount() const noexcept
{
    return resources_.memoryCount;
}

const MemoryRange* HardwareResources::MemoryRangeAt(_In_ ULONG index) const noexcept
{
    return index < resources_.memoryCount ? &resources_.memory[index] : nullptr;
}

ULONG HardwareResources::InterruptCount() const noexcept
{
    return resources_.interruptCount;
}

const InterruptResource* HardwareResources::InterruptAt(_In_ ULONG index) const noexcept
{
    return index < resources_.interruptCount ? &resources_.interrupts[index] : nullptr;
}

} // namespace nv::kmdod