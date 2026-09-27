#include "nvplatform_mmio_user.h"
#include "nvplatform_mmio_rm.h"

#include "kernel/os/nv_memory_type.h"

namespace {

constexpr ULONG MaximumApertures = 32;
constexpr NTSTATUS StatusSuccess = 0;
constexpr NTSTATUS StatusInvalidParameter = static_cast<NTSTATUS>(0xC000000DL);
constexpr NTSTATUS StatusInsufficientResources = static_cast<NTSTATUS>(0xC000009AL);

struct Aperture
{
    NvU64 physicalAddress;
    volatile NvU8* address;
    SIZE_T length;
    bool registered;
};

SRWLOCK apertureLock = SRWLOCK_INIT;
Aperture apertures[MaximumApertures];

Aperture* ApertureFromHandle(void* handle) noexcept
{
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(handle);
    const ULONG_PTR first = reinterpret_cast<ULONG_PTR>(&apertures[0]);
    const ULONG_PTR end = reinterpret_cast<ULONG_PTR>(&apertures[MaximumApertures]);
    return value >= first && value < end &&
        (value - first) % sizeof(Aperture) == 0
        ? static_cast<Aperture*>(handle)
        : nullptr;
}

bool IsSupportedProtection(NvU32 protect, NvU32 cacheType) noexcept
{
    return protect != 0 && (protect & ~NV_PROTECT_READ_WRITE) == 0 &&
        (cacheType == NV_MEMORY_DEFAULT ||
         cacheType == NV_MEMORY_UNCACHED ||
         cacheType == NV_MEMORY_UNCACHED_WEAK);
}

} // namespace

extern "C" NTSTATUS NvPlatformRegisterMmioAperture(
    ULONGLONG physicalAddress,
    volatile void* address,
    SIZE_T length,
    void** registration) noexcept
{
    if (physicalAddress == 0 || address == nullptr || length == 0 ||
        registration == nullptr)
    {
        return StatusInvalidParameter;
    }
    *registration = nullptr;

    AcquireSRWLockExclusive(&apertureLock);
    Aperture* available = nullptr;
    for (auto& aperture : apertures)
    {
        if (!aperture.registered && available == nullptr)
        {
            available = &aperture;
        }
    }
    if (available != nullptr)
    {
        available->physicalAddress = physicalAddress;
        available->address = static_cast<volatile NvU8*>(address);
        available->length = length;
        available->registered = true;
        *registration = available;
    }
    ReleaseSRWLockExclusive(&apertureLock);
    return available != nullptr ? StatusSuccess : StatusInsufficientResources;
}

extern "C" void NvPlatformUnregisterMmioAperture(void* registration) noexcept
{
    Aperture* aperture = ApertureFromHandle(registration);
    if (aperture == nullptr)
    {
        return;
    }
    AcquireSRWLockExclusive(&apertureLock);
    ZeroMemory(aperture, sizeof(*aperture));
    ReleaseSRWLockExclusive(&apertureLock);
}

extern "C" NV_STATUS NvPlatformRmMapPhysical(
    NvU64 physicalAddress,
    NvU64 length,
    NvU32 protect,
    NvU32 cacheType,
    void** virtualAddress)
{
    if (length == 0 || length > MAXSIZE_T || virtualAddress == nullptr ||
        !IsSupportedProtection(protect, cacheType))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *virtualAddress = nullptr;

    AcquireSRWLockShared(&apertureLock);
    for (const auto& aperture : apertures)
    {
        if (!aperture.registered || physicalAddress < aperture.physicalAddress)
        {
            continue;
        }
        const NvU64 offset = physicalAddress - aperture.physicalAddress;
        if (offset <= aperture.length && length <= aperture.length - offset)
        {
            *virtualAddress = const_cast<NvU8*>(aperture.address + offset);
            break;
        }
    }
    ReleaseSRWLockShared(&apertureLock);
    return *virtualAddress != nullptr ? NV_OK : NV_ERR_OBJECT_NOT_FOUND;
}

extern "C" void NvPlatformRmUnmapPhysical(void*)
{
}

extern "C" void NvPlatformRmWriteRegister8(volatile void* address, NvU8 value)
{
    MemoryBarrier();
    *static_cast<volatile NvU8*>(address) = value;
    MemoryBarrier();
}

extern "C" void NvPlatformRmWriteRegister16(volatile void* address, NvU16 value)
{
    MemoryBarrier();
    *static_cast<volatile NvU16*>(address) = value;
    MemoryBarrier();
}

extern "C" void NvPlatformRmWriteRegister32(volatile void* address, NvU32 value)
{
    MemoryBarrier();
    *static_cast<volatile NvU32*>(address) = value;
    MemoryBarrier();
}

extern "C" NvU8 NvPlatformRmReadRegister8(const volatile void* address)
{
    const NvU8 value = *static_cast<const volatile NvU8*>(address);
    MemoryBarrier();
    return value;
}

extern "C" NvU16 NvPlatformRmReadRegister16(const volatile void* address)
{
    const NvU16 value = *static_cast<const volatile NvU16*>(address);
    MemoryBarrier();
    return value;
}

extern "C" NvU32 NvPlatformRmReadRegister32(const volatile void* address)
{
    const NvU32 value = *static_cast<const volatile NvU32*>(address);
    MemoryBarrier();
    return value;
}