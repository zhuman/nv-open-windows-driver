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
            location_{},
            memoryBarAddresses_{},
            memoryBarCount_(0),
    registerBarAddress_{},
    configAccess_{},
            configRegistration_(nullptr),
      present_(false)
{
}

NTSTATUS PciDevice::Initialize(
        _In_ DEVICE_OBJECT* physicalDeviceObject,
    _In_ const DXGKRNL_INTERFACE& dxgkInterface,
    _In_ bool configurationRequired) noexcept
{
    Reset();

    if (!configurationRequired)
    {
        return STATUS_SUCCESS;
    }

    if (physicalDeviceObject == nullptr || dxgkInterface.DxgkCbReadDeviceSpace == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
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

    PHYSICAL_ADDRESS memoryBarAddresses[MaximumGpuMemoryBarCount] = {};
    ULONG memoryBarCount = 0;
    for (ULONG barIndex = 0;
         barIndex < PCI_TYPE0_ADDRESSES && memoryBarCount < MaximumGpuMemoryBarCount;
         ++barIndex)
    {
        const ULONG barLow = configuration.u.type0.BaseAddresses[barIndex];
        if (barLow == 0 || (barLow & PCI_ADDRESS_IO_SPACE) != 0)
        {
            continue;
        }

        ULONGLONG barAddress = barLow & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
        if ((barLow & PCI_ADDRESS_MEMORY_TYPE_MASK) == PCI_TYPE_64BIT)
        {
            if (++barIndex == PCI_TYPE0_ADDRESSES)
            {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            barAddress |=
                static_cast<ULONGLONG>(configuration.u.type0.BaseAddresses[barIndex]) << 32;
        }

        if (barAddress != 0)
        {
            memoryBarAddresses[memoryBarCount++].QuadPart = barAddress;
        }
    }

    if (memoryBarCount == 0)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    PCI_SEGMENT_BUS_NUMBER segmentBus = {};
    ULONG propertyLength = 0;
    NTSTATUS propertyStatus = IoGetDeviceProperty(
        physicalDeviceObject,
        DevicePropertyBusNumber,
        sizeof(segmentBus.u.AsULONG),
        &segmentBus.u.AsULONG,
        &propertyLength);
    if (!NT_SUCCESS(propertyStatus) ||
        propertyLength != sizeof(segmentBus.u.AsULONG) ||
        segmentBus.u.bits.Reserved != 0)
    {
        return NT_SUCCESS(propertyStatus)
            ? STATUS_DEVICE_CONFIGURATION_ERROR
            : propertyStatus;
    }

    PCI_SLOT_NUMBER slot = {};
    propertyStatus = IoGetDeviceProperty(
        physicalDeviceObject,
        DevicePropertyAddress,
        sizeof(slot.u.AsULONG),
        &slot.u.AsULONG,
        &propertyLength);
    if (!NT_SUCCESS(propertyStatus) ||
        propertyLength != sizeof(slot.u.AsULONG) ||
        slot.u.bits.Reserved != 0)
    {
        return NT_SUCCESS(propertyStatus)
            ? STATUS_DEVICE_CONFIGURATION_ERROR
            : propertyStatus;
    }

    NV_PLATFORM_PCI_LOCATION location = {};
    location.domain = segmentBus.u.bits.SegmentNumber;
    location.bus = static_cast<UCHAR>(segmentBus.u.bits.BusNumber);
    location.device = static_cast<UCHAR>(slot.u.bits.DeviceNumber);
    location.function = static_cast<UCHAR>(slot.u.bits.FunctionNumber);

    void* configRegistration = nullptr;
    propertyStatus = NvPlatformRegisterPciConfig(
        &location,
        &configAccess,
        &configRegistration);
    if (!NT_SUCCESS(propertyStatus))
    {
        return propertyStatus;
    }

    identity_.vendorId = configuration.VendorID;
    identity_.deviceId = configuration.DeviceID;
    identity_.subsystemVendorId = configuration.u.type0.SubVendorID;
    identity_.subsystemId = configuration.u.type0.SubSystemID;
    identity_.revisionId = configuration.RevisionID;
    identity_.programmingInterface = configuration.ProgIf;
    identity_.subclass = configuration.SubClass;
    identity_.baseClass = configuration.BaseClass;
    location_ = location;
    RtlCopyMemory(
        memoryBarAddresses_,
        memoryBarAddresses,
        memoryBarCount * sizeof(memoryBarAddresses_[0]));
    memoryBarCount_ = memoryBarCount;
    registerBarAddress_ = memoryBarAddresses_[0];
    configAccess_ = configAccess;
    configRegistration_ = configRegistration;
    present_ = true;
    return STATUS_SUCCESS;
}

void PciDevice::Reset() noexcept
{
    NvPlatformUnregisterPciConfig(configRegistration_);
    RtlZeroMemory(&identity_, sizeof(identity_));
    RtlZeroMemory(&location_, sizeof(location_));
    RtlZeroMemory(memoryBarAddresses_, sizeof(memoryBarAddresses_));
    memoryBarCount_ = 0;
    registerBarAddress_.QuadPart = 0;
    RtlZeroMemory(&configAccess_, sizeof(configAccess_));
    configRegistration_ = nullptr;
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

const NV_PLATFORM_PCI_LOCATION* PciDevice::Location() const noexcept
{
    return present_ ? &location_ : nullptr;
}

ULONG PciDevice::MemoryBarCount() const noexcept
{
    return present_ ? memoryBarCount_ : 0;
}

PHYSICAL_ADDRESS PciDevice::MemoryBarAddress(_In_ ULONG index) const noexcept
{
    PHYSICAL_ADDRESS address = {};
    return present_ && index < memoryBarCount_ ? memoryBarAddresses_[index] : address;
}

PHYSICAL_ADDRESS PciDevice::RegisterBarAddress() const noexcept
{
    return registerBarAddress_;
}

} // namespace nv::kmdod