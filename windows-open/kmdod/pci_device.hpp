#pragma once

#include "wdk.hpp"

namespace nv::kmdod {

struct PciIdentity final
{
    USHORT vendorId;
    USHORT deviceId;
    USHORT subsystemVendorId;
    USHORT subsystemId;
    UCHAR revisionId;
    UCHAR programmingInterface;
    UCHAR subclass;
    UCHAR baseClass;
};

class PciDevice final
{
public:
    PciDevice() noexcept;

    NTSTATUS Initialize(
        _In_ const DXGKRNL_INTERFACE& dxgkInterface,
        _In_ bool configurationRequired) noexcept;

    void Reset() noexcept;

    [[nodiscard]] bool IsPresent() const noexcept;
    [[nodiscard]] const PciIdentity* Identity() const noexcept;

private:
    PciIdentity identity_;
    bool present_;
};

} // namespace nv::kmdod