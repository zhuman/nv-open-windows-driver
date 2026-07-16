#include "pci_device.hpp"

namespace {

constexpr USHORT NvidiaVendorId = 0x10DE;

NTSTATUS ReadConfig(
    _In_ void* context,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG offset,
    _In_ ULONG length,
    _Out_ ULONG* bytesRead)
{
    const auto* dxgkInterface = static_cast<const DXGKRNL_INTERFACE*>(context);
    return dxgkInterface->DxgkCbReadDeviceSpace(
        dxgkInterface->DeviceHandle,
        DXGK_WHICHSPACE_CONFIG,
        buffer,
        offset,
        length,
        bytesRead);
}

NTSTATUS WriteConfig(
    _In_ void* context,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG offset,
    _In_ ULONG length,
    _Out_ ULONG* bytesWritten)
{
    const auto* dxgkInterface = static_cast<const DXGKRNL_INTERFACE*>(context);
    return dxgkInterface->DxgkCbWriteDeviceSpace(
        dxgkInterface->DeviceHandle,
        DXGK_WHICHSPACE_CONFIG,
        const_cast<void*>(buffer),
        offset,
        length,
        bytesWritten);
}

} // namespace

namespace nv::kmdod {

PciDevice::PciDevice() noexcept
    : identity_{},
    registerBarAddress_{},
    configAccess_{},
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

    NV_PLATFORM_PCI_CONFIG_ACCESS configAccess = {};
    configAccess.context = const_cast<DXGKRNL_INTERFACE*>(&dxgkInterface);
    configAccess.read = ::ReadConfig;
    configAccess.write = dxgkInterface.DxgkCbWriteDeviceSpace != nullptr
        ? ::WriteConfig
        : nullptr;

    PCI_COMMON_CONFIG configuration = {};
    const NTSTATUS status = NvPlatformReadPciConfig(
        &configAccess,
        0,
        &configuration,
        PCI_COMMON_HDR_LENGTH);
    if (!NT_SUCCESS(status))
    {
        return status;
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
    configAccess_ = configAccess;
    present_ = true;
    return STATUS_SUCCESS;
}

void PciDevice::Reset() noexcept
{
    RtlZeroMemory(&identity_, sizeof(identity_));
    registerBarAddress_.QuadPart = 0;
    RtlZeroMemory(&configAccess_, sizeof(configAccess_));
    present_ = false;
}

NTSTATUS PciDevice::ReadConfig(
    _In_ ULONG offset,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG length) const noexcept
{
    return present_
        ? NvPlatformReadPciConfig(&configAccess_, offset, buffer, length)
        : STATUS_INVALID_DEVICE_STATE;
}

NTSTATUS PciDevice::WriteConfig(
    _In_ ULONG offset,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG length) const noexcept
{
    return present_
        ? NvPlatformWritePciConfig(&configAccess_, offset, buffer, length)
        : STATUS_INVALID_DEVICE_STATE;
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