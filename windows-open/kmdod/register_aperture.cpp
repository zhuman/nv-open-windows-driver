#include "register_aperture.hpp"

namespace {

constexpr ULONG PmcBoot0Offset = 0x00000000;
constexpr ULONG PmcBoot42Offset = 0x00000A00;
constexpr SIZE_T IdentityMappingLength = PAGE_SIZE;

} // namespace

namespace nv::kmdod {

RegisterAperture::RegisterAperture() noexcept
    : registers_(nullptr),
      mappedLength_(0),
      identity_{}
{
}

NTSTATUS RegisterAperture::Initialize(
    _In_ const HardwareResources& hardwareResources,
    _In_ const PciDevice& pciDevice) noexcept
{
    Reset();

    if (!pciDevice.IsPresent())
    {
        return STATUS_SUCCESS;
    }

    const PHYSICAL_ADDRESS registerBarAddress = pciDevice.RegisterBarAddress();
    const MemoryRange* registerRange = nullptr;
    for (ULONG index = 0; index < hardwareResources.MemoryRangeCount(); ++index)
    {
        const MemoryRange* range = hardwareResources.MemoryRangeAt(index);
        if (range != nullptr &&
            range->physicalAddress.QuadPart == registerBarAddress.QuadPart)
        {
            registerRange = range;
            break;
        }
    }

    if (registerRange == nullptr || registerRange->length < IdentityMappingLength)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    registers_ = static_cast<volatile UCHAR*>(MmMapIoSpaceEx(
        registerRange->physicalAddress,
        IdentityMappingLength,
        PAGE_READWRITE | PAGE_NOCACHE));
    if (registers_ == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mappedLength_ = IdentityMappingLength;
    identity_.boot0 = READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG*>(registers_ + PmcBoot0Offset));
    identity_.boot42 = READ_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG*>(registers_ + PmcBoot42Offset));

    if (identity_.boot0 == MAXULONG && identity_.boot42 == MAXULONG)
    {
        Reset();
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    return STATUS_SUCCESS;
}

void RegisterAperture::Reset() noexcept
{
    if (registers_ != nullptr)
    {
        MmUnmapIoSpace(const_cast<UCHAR*>(registers_), mappedLength_);
    }

    registers_ = nullptr;
    mappedLength_ = 0;
    RtlZeroMemory(&identity_, sizeof(identity_));
}

bool RegisterAperture::IsMapped() const noexcept
{
    return registers_ != nullptr;
}

const BootIdentity* RegisterAperture::Identity() const noexcept
{
    return IsMapped() ? &identity_ : nullptr;
}

} // namespace nv::kmdod