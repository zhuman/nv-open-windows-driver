#pragma once

#include "wdk.hpp"

namespace nv::kmdod {

struct MemoryRange final
{
    PHYSICAL_ADDRESS physicalAddress;
    SIZE_T length;
    USHORT flags;
};

struct InterruptResource final
{
    ULONG vector;
    ULONG level;
    KAFFINITY affinity;
    USHORT flags;
    bool messageSignaled;
};

class HardwareResources final
{
public:
    static constexpr ULONG MaximumMemoryRangeCount = 16;
    static constexpr ULONG MaximumInterruptCount = 32;

    HardwareResources() noexcept;

    NTSTATUS Initialize(_In_opt_ const CM_RESOURCE_LIST* translatedResources) noexcept;
    void Reset() noexcept;

    [[nodiscard]] ULONG MemoryRangeCount() const noexcept;
    [[nodiscard]] const MemoryRange* MemoryRangeAt(_In_ ULONG index) const noexcept;
    [[nodiscard]] ULONG InterruptCount() const noexcept;
    [[nodiscard]] const InterruptResource* InterruptAt(_In_ ULONG index) const noexcept;

private:
    NTSTATUS AddMemoryRange(
        _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor) noexcept;

    static NTSTATUS DecodeMemoryRange(
        _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor,
        _Out_ PHYSICAL_ADDRESS* physicalAddress,
        _Out_ SIZE_T* length) noexcept;

    NTSTATUS AddInterrupt(
        _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR& descriptor) noexcept;

    MemoryRange memoryRanges_[MaximumMemoryRangeCount];
    ULONG memoryRangeCount_;
    InterruptResource interrupts_[MaximumInterruptCount];
    ULONG interruptCount_;
};

} // namespace nv::kmdod