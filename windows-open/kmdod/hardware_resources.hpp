#pragma once

#include "nvplatform_resources.h"
#include "wdk.hpp"

namespace nv::kmdod {

using MemoryRange = NV_PLATFORM_MEMORY_RESOURCE;
using InterruptResource = NV_PLATFORM_INTERRUPT_RESOURCE;

class HardwareResources final
{
public:
    static constexpr ULONG MaximumMemoryRangeCount = NV_PLATFORM_MAX_MEMORY_RESOURCE_COUNT;
    static constexpr ULONG MaximumInterruptCount = NV_PLATFORM_MAX_INTERRUPT_RESOURCE_COUNT;

    HardwareResources() noexcept;

    NTSTATUS Initialize(_In_opt_ const CM_RESOURCE_LIST* translatedResources) noexcept;
    void Reset() noexcept;

    [[nodiscard]] ULONG MemoryRangeCount() const noexcept;
    [[nodiscard]] const MemoryRange* MemoryRangeAt(_In_ ULONG index) const noexcept;
    [[nodiscard]] ULONG InterruptCount() const noexcept;
    [[nodiscard]] const InterruptResource* InterruptAt(_In_ ULONG index) const noexcept;

private:
    NV_PLATFORM_TRANSLATED_RESOURCES resources_;
};

} // namespace nv::kmdod