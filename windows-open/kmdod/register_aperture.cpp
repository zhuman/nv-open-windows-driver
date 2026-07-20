#include "register_aperture.hpp"

namespace {

constexpr ULONG PmcBoot0Offset = 0x00000000;
constexpr ULONG PmcBoot42Offset = 0x00000A00;
constexpr SIZE_T RequiredIdentityLength = PmcBoot42Offset + sizeof(ULONG);

} // namespace

namespace nv::kmdod {

RegisterAperture::RegisterAperture() noexcept
    : mapping_{},
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

    if (registerRange == nullptr || registerRange->length < RequiredIdentityLength)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    NTSTATUS status = NvPlatformMapMmio(
        registerRange->physicalAddress,
        registerRange->length,
        &mapping_);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = Read32(PmcBoot0Offset, &identity_.boot0);
    if (NT_SUCCESS(status))
    {
        status = Read32(PmcBoot42Offset, &identity_.boot42);
    }

    if (!NT_SUCCESS(status))
    {
        Reset();
        return status;
    }

    if (identity_.boot0 == MAXULONG && identity_.boot42 == MAXULONG)
    {
        Reset();
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    return STATUS_SUCCESS;
}

void RegisterAperture::Reset() noexcept
{
    NvPlatformUnmapMmio(&mapping_);
    RtlZeroMemory(&identity_, sizeof(identity_));
}

NTSTATUS RegisterAperture::Read32(
    _In_ SIZE_T offset,
    _Out_ ULONG* value) const noexcept
{
    return NvPlatformReadMmio32(&mapping_, offset, value);
}

bool RegisterAperture::IsMapped() const noexcept
{
    return mapping_.address != nullptr;
}

volatile void* RegisterAperture::Address() const noexcept
{
    return mapping_.address;
}

SIZE_T RegisterAperture::Length() const noexcept
{
    return mapping_.length;
}

const BootIdentity* RegisterAperture::Identity() const noexcept
{
    return IsMapped() ? &identity_ : nullptr;
}

} // namespace nv::kmdod