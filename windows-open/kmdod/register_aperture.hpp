#pragma once

#include "hardware_resources.hpp"
#include "nvplatform_mmio.h"
#include "pci_device.hpp"
#include "wdk.hpp"

namespace nv::kmdod {

struct BootIdentity final
{
    ULONG boot0;
    ULONG boot42;
};

class RegisterAperture final
{
public:
    RegisterAperture() noexcept;

    NTSTATUS Initialize(
        _In_ const HardwareResources& hardwareResources,
        _In_ const PciDevice& pciDevice) noexcept;

    void Reset() noexcept;

    NTSTATUS Read32(_In_ SIZE_T offset, _Out_ ULONG* value) const noexcept;

    [[nodiscard]] bool IsMapped() const noexcept;
    [[nodiscard]] SIZE_T Length() const noexcept;
    [[nodiscard]] const BootIdentity* Identity() const noexcept;

private:
    NV_PLATFORM_MMIO_MAPPING mapping_;
    BootIdentity identity_;
};

} // namespace nv::kmdod