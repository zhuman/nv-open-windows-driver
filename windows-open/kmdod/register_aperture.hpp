#pragma once

#include "hardware_resources.hpp"
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

    [[nodiscard]] bool IsMapped() const noexcept;
    [[nodiscard]] const BootIdentity* Identity() const noexcept;

private:
    volatile UCHAR* registers_;
    SIZE_T mappedLength_;
    BootIdentity identity_;
};

} // namespace nv::kmdod