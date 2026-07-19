#include <initguid.h>
#include <wdmguid.h>

#include "hardware_services.hpp"

namespace {

constexpr USHORT BusInterfaceVersion = 1;
constexpr USHORT NvidiaVendorId = 0x10DE;
constexpr size_t MaximumDmaTransferLength = 0xFFFFFFFFull;
constexpr ULONG PmcBoot0Offset = 0x00000000;
constexpr ULONG PmcBoot42Offset = 0x00000A00;
constexpr SIZE_T RequiredIdentityLength = PmcBoot42Offset + sizeof(ULONG);
constexpr ULONG PciBarAddressMask = ~0x0Ful;
constexpr ULONG PciBarMemoryTypeMask = 0x6;
constexpr ULONG PciBarMemoryType64 = 0x4;
constexpr ULONG InvalidRegisterValue = static_cast<ULONG>(-1);

struct PciCommonHeader final
{
    USHORT vendorId;
    USHORT deviceId;
    USHORT command;
    USHORT status;
    UCHAR revisionId;
    UCHAR programmingInterface;
    UCHAR subclass;
    UCHAR baseClass;
    UCHAR cacheLineSize;
    UCHAR latencyTimer;
    UCHAR headerType;
    UCHAR bist;
    ULONG baseAddresses[6];
    ULONG cardbusCisPointer;
    USHORT subsystemVendorId;
    USHORT subsystemId;
};

static_assert(sizeof(PciCommonHeader) == 0x30);

} // namespace

namespace nv::iddcx {

NTSTATUS ConfigSpace::Initialize(_In_ WDFDEVICE device) noexcept
{
    Reset();

    NTSTATUS status = WdfFdoQueryForInterface(
        device,
        &GUID_BUS_INTERFACE_STANDARD,
        reinterpret_cast<PINTERFACE>(&interface_),
        sizeof(interface_),
        BusInterfaceVersion,
        nullptr);
    if (!NT_SUCCESS(status))
    {
        Reset();
        return status;
    }

    if (interface_.GetBusData == nullptr || interface_.SetBusData == nullptr)
    {
        Reset();
        return STATUS_NOT_SUPPORTED;
    }

    PciCommonHeader header = {};
    status = Read(0, &header, sizeof(header));
    if (!NT_SUCCESS(status))
    {
        Reset();
        return status;
    }

    if (header.vendorId == 0 || header.vendorId == 0xFFFF)
    {
        Reset();
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    if (header.vendorId != NvidiaVendorId)
    {
        Reset();
        return STATUS_NOT_SUPPORTED;
    }

    if ((header.baseAddresses[0] & 0x1) != 0)
    {
        Reset();
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ULONGLONG registerBarAddress = header.baseAddresses[0] & PciBarAddressMask;
    if ((header.baseAddresses[0] & PciBarMemoryTypeMask) == PciBarMemoryType64)
    {
        registerBarAddress |= static_cast<ULONGLONG>(header.baseAddresses[1]) << 32;
    }

    if (registerBarAddress == 0)
    {
        Reset();
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    identity_.vendorId = header.vendorId;
    identity_.deviceId = header.deviceId;
    identity_.subsystemVendorId = header.subsystemVendorId;
    identity_.subsystemId = header.subsystemId;
    identity_.revisionId = header.revisionId;
    identity_.programmingInterface = header.programmingInterface;
    identity_.subclass = header.subclass;
    identity_.baseClass = header.baseClass;
    registerBarAddress_.QuadPart = registerBarAddress;
    identityValid_ = true;

    return STATUS_SUCCESS;
}

void ConfigSpace::Reset() noexcept
{
    if (interface_.InterfaceDereference != nullptr)
    {
        interface_.InterfaceDereference(interface_.Context);
    }

    RtlZeroMemory(&interface_, sizeof(interface_));
    RtlZeroMemory(&identity_, sizeof(identity_));
    registerBarAddress_.QuadPart = 0;
    identityValid_ = false;
}

NTSTATUS ConfigSpace::Read(
    _In_ ULONG offset,
    _Out_writes_bytes_(length) void* buffer,
    _In_ ULONG length) const noexcept
{
    if (!IsAvailable() || buffer == nullptr || length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const ULONG bytesRead = interface_.GetBusData(
        interface_.Context,
        PCI_WHICHSPACE_CONFIG,
        buffer,
        offset,
        length);
    return bytesRead == length ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

NTSTATUS ConfigSpace::Write(
    _In_ ULONG offset,
    _In_reads_bytes_(length) const void* buffer,
    _In_ ULONG length) const noexcept
{
    if (!IsAvailable() || buffer == nullptr || length == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const ULONG bytesWritten = interface_.SetBusData(
        interface_.Context,
        PCI_WHICHSPACE_CONFIG,
        const_cast<void*>(buffer),
        offset,
        length);
    return bytesWritten == length ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

bool ConfigSpace::IsAvailable() const noexcept
{
    return interface_.GetBusData != nullptr && interface_.SetBusData != nullptr;
}

const PciIdentity* ConfigSpace::Identity() const noexcept
{
    return identityValid_ ? &identity_ : nullptr;
}

PHYSICAL_ADDRESS ConfigSpace::RegisterBarAddress() const noexcept
{
    return registerBarAddress_;
}

NTSTATUS RegisterAperture::Initialize(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesTranslated,
    _In_ const ConfigSpace& configSpace) noexcept
{
    Reset();

    const PHYSICAL_ADDRESS registerBarAddress = configSpace.RegisterBarAddress();
    const ULONG resourceCount = WdfCmResourceListGetCount(resourcesTranslated);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR registerRange = nullptr;
    for (ULONG index = 0; index < resourceCount; ++index)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resource =
            WdfCmResourceListGetDescriptor(resourcesTranslated, index);
        if (resource != nullptr &&
            resource->Type == CmResourceTypeMemory &&
            resource->u.Memory.Start.QuadPart == registerBarAddress.QuadPart)
        {
            registerRange = resource;
            break;
        }
    }

    if (registerRange == nullptr || registerRange->u.Memory.Length < RequiredIdentityLength)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    NTSTATUS status = WdfDeviceMapIoSpace(
        device,
        registerRange->u.Memory.Start,
        registerRange->u.Memory.Length,
        MmNonCached,
        &address_);
    if (!NT_SUCCESS(status))
    {
        Reset();
        return status;
    }

    device_ = device;
    length_ = registerRange->u.Memory.Length;
    status = Read32(PmcBoot0Offset, &identity_.boot0);
    if (NT_SUCCESS(status))
    {
        status = Read32(PmcBoot42Offset, &identity_.boot42);
    }

    if (!NT_SUCCESS(status) ||
        (identity_.boot0 == InvalidRegisterValue &&
         identity_.boot42 == InvalidRegisterValue))
    {
        Reset();
        return NT_SUCCESS(status) ? STATUS_DEVICE_DOES_NOT_EXIST : status;
    }

    return STATUS_SUCCESS;
}

void RegisterAperture::Reset() noexcept
{
    if (device_ != nullptr && address_ != nullptr)
    {
        WdfDeviceUnmapIoSpace(device_, address_, length_);
    }

    device_ = nullptr;
    address_ = nullptr;
    length_ = 0;
    RtlZeroMemory(&identity_, sizeof(identity_));
}

NTSTATUS RegisterAperture::Read32(
    _In_ SIZE_T offset,
    _Out_ ULONG* value) const noexcept
{
    if (!IsMapped() || value == nullptr ||
        offset > length_ || sizeof(*value) > length_ - offset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *value = WDF_READ_REGISTER_ULONG(
        device_,
        reinterpret_cast<PULONG>(static_cast<UCHAR*>(address_) + offset));
    return STATUS_SUCCESS;
}

bool RegisterAperture::IsMapped() const noexcept
{
    return address_ != nullptr;
}

SIZE_T RegisterAperture::Length() const noexcept
{
    return length_;
}

const BootIdentity* RegisterAperture::Identity() const noexcept
{
    return IsMapped() ? &identity_ : nullptr;
}

NTSTATUS DmaEngine::Initialize(_In_ WDFDEVICE device) noexcept
{
    Reset();

    WDF_DMA_ENABLER_CONFIG configuration;
    WDF_DMA_ENABLER_CONFIG_INIT(
        &configuration,
        WdfDmaProfileScatterGather64Duplex,
        MaximumDmaTransferLength);
    configuration.Flags = WDF_DMA_ENABLER_CONFIG_NO_SGLIST_PREALLOCATION;

    const NTSTATUS status = WdfDmaEnablerCreate(
        device,
        &configuration,
        WDF_NO_OBJECT_ATTRIBUTES,
        &handle_);
    if (!NT_SUCCESS(status))
    {
        Reset();
    }

    return status;
}

void DmaEngine::Reset() noexcept
{
    if (handle_ != nullptr)
    {
        WdfObjectDelete(handle_);
        handle_ = nullptr;
    }
}

WDFDMAENABLER DmaEngine::Handle() const noexcept
{
    return handle_;
}

NTSTATUS Interrupt::Initialize(
    _In_ WDFDEVICE device,
    _In_ WDFCMRESLIST resourcesRaw,
    _In_ WDFCMRESLIST resourcesTranslated) noexcept
{
    Reset();

    const ULONG translatedCount = WdfCmResourceListGetCount(resourcesTranslated);
    const ULONG rawCount = WdfCmResourceListGetCount(resourcesRaw);
    if (translatedCount != rawCount)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (ULONG index = 0; index < translatedCount; ++index)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translated =
            WdfCmResourceListGetDescriptor(resourcesTranslated, index);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw =
            WdfCmResourceListGetDescriptor(resourcesRaw, index);

        if (translated == nullptr || raw == nullptr ||
            translated->Type != CmResourceTypeInterrupt ||
            raw->Type != CmResourceTypeInterrupt)
        {
            continue;
        }

        WDF_INTERRUPT_CONFIG configuration;
        WDF_INTERRUPT_CONFIG_INIT(&configuration, Isr, Dpc);
        configuration.InterruptRaw = raw;
        configuration.InterruptTranslated = translated;

        const NTSTATUS status = WdfInterruptCreate(
            device,
            &configuration,
            WDF_NO_OBJECT_ATTRIBUTES,
            &handle_);
        if (!NT_SUCCESS(status))
        {
            Reset();
        }

        return status;
    }

    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

void Interrupt::Reset() noexcept
{
    if (handle_ != nullptr)
    {
        WdfObjectDelete(handle_);
        handle_ = nullptr;
    }
}

WDFINTERRUPT Interrupt::Handle() const noexcept
{
    return handle_;
}

_Use_decl_annotations_
BOOLEAN Interrupt::Isr(WDFINTERRUPT interrupt, ULONG messageId)
{
    UNREFERENCED_PARAMETER(interrupt);
    UNREFERENCED_PARAMETER(messageId);
    return FALSE;
}

_Use_decl_annotations_
void Interrupt::Dpc(WDFINTERRUPT interrupt, WDFOBJECT associatedObject)
{
    UNREFERENCED_PARAMETER(interrupt);
    UNREFERENCED_PARAMETER(associatedObject);
}

} // namespace nv::iddcx