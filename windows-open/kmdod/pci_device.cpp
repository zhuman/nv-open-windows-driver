#include "pci_device.hpp"

namespace {

constexpr USHORT NvidiaVendorId = 0x10DE;

} // namespace

namespace nv::kmdod {

PciDevice::PciDevice() noexcept
    : identity_{},
    registerBarAddress_{},
      present_(false)
{
}

NTSTATUS PciDevice::Initialize(
    _In_ const DXGKRNL_INTERFACE& dxgkInterface,
    _In_ bool configurationRequired) noexcept
{
    Reset();

    if (!configurationRequired)
    {
        return STATUS_SUCCESS;
    }

    if (dxgkInterface.DxgkCbReadDeviceSpace == nullptr)
    {
        return STATUS_NOT_SUPPORTED;
    }

    PCI_COMMON_CONFIG configuration = {};
    ULONG bytesRead = 0;
    const NTSTATUS status = dxgkInterface.DxgkCbReadDeviceSpace(
        dxgkInterface.DeviceHandle,
        DXGK_WHICHSPACE_CONFIG,
        &configuration,
        0,
        PCI_COMMON_HDR_LENGTH,
        &bytesRead);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (bytesRead != PCI_COMMON_HDR_LENGTH)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (configuration.VendorID == PCI_INVALID_VENDORID || configuration.VendorID == 0)
    {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    if (configuration.VendorID != NvidiaVendorId)
    {
        return STATUS_NOT_SUPPORTED;
    }

    const ULONG registerBarLow = configuration.u.type0.BaseAddresses[0];
    if ((registerBarLow & PCI_ADDRESS_IO_SPACE) != 0)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ULONGLONG registerBarAddress = registerBarLow & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    if ((registerBarLow & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT)
    {
        registerBarAddress |=
            static_cast<ULONGLONG>(configuration.u.type0.BaseAddresses[1]) << 32;
    }

    if (registerBarAddress == 0)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    identity_.vendorId = configuration.VendorID;
    identity_.deviceId = configuration.DeviceID;
    identity_.subsystemVendorId = configuration.u.type0.SubVendorID;
    identity_.subsystemId = configuration.u.type0.SubSystemID;
    identity_.revisionId = configuration.RevisionID;
    identity_.programmingInterface = configuration.ProgIf;
    identity_.subclass = configuration.SubClass;
    identity_.baseClass = configuration.BaseClass;
    registerBarAddress_.QuadPart = registerBarAddress;
    present_ = true;
    return STATUS_SUCCESS;
}

void PciDevice::Reset() noexcept
{
    RtlZeroMemory(&identity_, sizeof(identity_));
    registerBarAddress_.QuadPart = 0;
    present_ = false;
}

bool PciDevice::IsPresent() const noexcept
{
    return present_;
}

const PciIdentity* PciDevice::Identity() const noexcept
{
    return present_ ? &identity_ : nullptr;
}

PHYSICAL_ADDRESS PciDevice::RegisterBarAddress() const noexcept
{
    return registerBarAddress_;
}

} // namespace nv::kmdod