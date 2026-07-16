#pragma once

#include "nvplatform_pci.h"
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

    NTSTATUS ReadConfig(
        _In_ ULONG offset,
        _Out_writes_bytes_(length) void* buffer,
        _In_ ULONG length) const noexcept;

    NTSTATUS WriteConfig(
        _In_ ULONG offset,
        _In_reads_bytes_(length) const void* buffer,
        _In_ ULONG length) const noexcept;

    [[nodiscard]] bool IsPresent() const noexcept;
    [[nodiscard]] const PciIdentity* Identity() const noexcept;
    [[nodiscard]] PHYSICAL_ADDRESS RegisterBarAddress() const noexcept;

private:
    PciIdentity identity_;
    PHYSICAL_ADDRESS registerBarAddress_;
    NV_PLATFORM_PCI_CONFIG_ACCESS configAccess_;
    bool present_;
};

} // namespace nv::kmdod