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
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType;
    NV_PLATFORM_MMIO_ACCESS access;
    ULONG* read32Cache;
    SIZE_T read32CacheOffset;
    NvU32 read32CacheCount;
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
         cacheType == NV_MEMORY_UNCACHED_WEAK ||
         cacheType == NV_MEMORY_WRITECOMBINED);
}

bool IsCompatibleCacheType(
    NV_PLATFORM_MMIO_CACHE_TYPE apertureCacheType,
    NvU32 requestedCacheType) noexcept
{
    return requestedCacheType == NV_MEMORY_DEFAULT ||
        (apertureCacheType == NV_PLATFORM_MMIO_CACHE_UNCACHED &&
         (requestedCacheType == NV_MEMORY_UNCACHED ||
          requestedCacheType == NV_MEMORY_UNCACHED_WEAK)) ||
        (apertureCacheType == NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED &&
         requestedCacheType == NV_MEMORY_WRITECOMBINED);
}

Aperture* FindAperture(
    const volatile void* address,
    SIZE_T width,
    SIZE_T* offset) noexcept
{
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(address);
    for (auto& aperture : apertures)
    {
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(aperture.address);
        if (!aperture.registered || value < base)
        {
            continue;
        }

        const ULONG_PTR candidateOffset = value - base;
        if (candidateOffset <= aperture.length &&
            width <= aperture.length - candidateOffset)
        {
            *offset = static_cast<SIZE_T>(candidateOffset);
            return &aperture;
        }
    }
    return nullptr;
}

} // namespace

extern "C" NTSTATUS NvPlatformRegisterMmioAperture(
    ULONGLONG physicalAddress,
    volatile void* address,
    SIZE_T length,
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
    const NV_PLATFORM_MMIO_ACCESS* access,
    void** registration) noexcept
{
    if (physicalAddress == 0 || address == nullptr || length == 0 ||
        (cacheType != NV_PLATFORM_MMIO_CACHE_UNCACHED &&
         cacheType != NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED) ||
        access == nullptr || access->context == nullptr ||
        access->read8 == nullptr || access->read16 == nullptr ||
        access->read32 == nullptr || access->read32Buffer == nullptr ||
        access->write8 == nullptr ||
        access->write16 == nullptr || access->write32 == nullptr ||
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
        available->cacheType = cacheType;
        available->access = *access;
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
    HeapFree(GetProcessHeap(), 0, aperture->read32Cache);
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
        if (!aperture.registered || physicalAddress < aperture.physicalAddress ||
            !IsCompatibleCacheType(aperture.cacheType, cacheType))
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

extern "C" NvBool NvPlatformRmCopyMemory(
    void* destination,
    const void* source,
    NvLength length)
{
    if (destination == nullptr || source == nullptr || length == 0)
    {
        return NV_FALSE;
    }

    AcquireSRWLockShared(&apertureLock);
    SIZE_T destinationOffset = 0;
    SIZE_T sourceOffset = 0;
    Aperture* destinationAperture = FindAperture(
        destination,
        static_cast<SIZE_T>(length),
        &destinationOffset);
    Aperture* sourceAperture = FindAperture(
        source,
        static_cast<SIZE_T>(length),
        &sourceOffset);
    if (destinationAperture == nullptr && sourceAperture == nullptr)
    {
        ReleaseSRWLockShared(&apertureLock);
        return NV_FALSE;
    }

    NvU8* destinationBytes = static_cast<NvU8*>(destination);
    const NvU8* sourceBytes = static_cast<const NvU8*>(source);
    for (NvLength index = 0; index < length; ++index)
    {
        const NvU8 value = sourceAperture != nullptr
            ? sourceAperture->access.read8(
                sourceAperture->access.context,
                sourceOffset + static_cast<SIZE_T>(index))
            : sourceBytes[index];
        if (destinationAperture != nullptr)
        {
            destinationAperture->access.write8(
                destinationAperture->access.context,
                destinationOffset + static_cast<SIZE_T>(index),
                value);
        }
        else
        {
            destinationBytes[index] = value;
        }
    }
    ReleaseSRWLockShared(&apertureLock);
    return NV_TRUE;
}

extern "C" void NvPlatformRmWriteRegister8(volatile void* address, NvU8 value)
{
    AcquireSRWLockShared(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(value), &offset);
    if (aperture != nullptr)
    {
        aperture->access.write8(aperture->access.context, offset, value);
    }
    ReleaseSRWLockShared(&apertureLock);
}

extern "C" void NvPlatformRmWriteRegister16(volatile void* address, NvU16 value)
{
    AcquireSRWLockShared(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(value), &offset);
    if (aperture != nullptr)
    {
        aperture->access.write16(aperture->access.context, offset, value);
    }
    ReleaseSRWLockShared(&apertureLock);
}

extern "C" void NvPlatformRmWriteRegister32(volatile void* address, NvU32 value)
{
    AcquireSRWLockShared(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(value), &offset);
    if (aperture != nullptr)
    {
        aperture->access.write32(aperture->access.context, offset, value);
    }
    ReleaseSRWLockShared(&apertureLock);
}

extern "C" NvU8 NvPlatformRmReadRegister8(const volatile void* address)
{
    AcquireSRWLockShared(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(NvU8), &offset);
    const NvU8 value = aperture != nullptr
        ? aperture->access.read8(aperture->access.context, offset)
        : NV_U8_MAX;
    ReleaseSRWLockShared(&apertureLock);
    return value;
}

extern "C" NvU16 NvPlatformRmReadRegister16(const volatile void* address)
{
    AcquireSRWLockShared(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(NvU16), &offset);
    const NvU16 value = aperture != nullptr
        ? aperture->access.read16(aperture->access.context, offset)
        : NV_U16_MAX;
    ReleaseSRWLockShared(&apertureLock);
    return value;
}

extern "C" NvU32 NvPlatformRmReadRegister32(const volatile void* address)
{
    AcquireSRWLockShared(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(NvU32), &offset);
    NvU32 value = NV_U32_MAX;
    if (aperture != nullptr)
    {
        if (aperture->read32Cache != nullptr &&
            offset >= aperture->read32CacheOffset &&
            (offset - aperture->read32CacheOffset) % sizeof(NvU32) == 0)
        {
            const SIZE_T index =
                (offset - aperture->read32CacheOffset) / sizeof(NvU32);
            value = index < aperture->read32CacheCount
                ? aperture->read32Cache[index]
                : aperture->access.read32(aperture->access.context, offset);
        }
        else
        {
            value = aperture->access.read32(aperture->access.context, offset);
        }
    }
    ReleaseSRWLockShared(&apertureLock);
    return value;
}

extern "C" NV_STATUS NvPlatformRmBeginRegisterCache32(
    const volatile void* address,
    NvU32 count)
{
    if (count == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    ULONG* cache = static_cast<ULONG*>(HeapAlloc(
        GetProcessHeap(), 0, static_cast<SIZE_T>(count) * sizeof(ULONG)));
    if (cache == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    NV_STATUS status = NV_ERR_INVALID_ADDRESS;
    AcquireSRWLockExclusive(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(
        address, static_cast<SIZE_T>(count) * sizeof(NvU32), &offset);
    if (aperture != nullptr && aperture->read32Cache == nullptr)
    {
        aperture->access.read32Buffer(
            aperture->access.context, offset, cache, count);
        aperture->read32Cache = cache;
        aperture->read32CacheOffset = offset;
        aperture->read32CacheCount = count;
        cache = nullptr;
        status = NV_OK;
    }
    else if (aperture != nullptr)
    {
        status = NV_ERR_STATE_IN_USE;
    }
    ReleaseSRWLockExclusive(&apertureLock);

    HeapFree(GetProcessHeap(), 0, cache);
    return status;
}

extern "C" void NvPlatformRmEndRegisterCache32(const volatile void* address)
{
    AcquireSRWLockExclusive(&apertureLock);
    SIZE_T offset = 0;
    Aperture* aperture = FindAperture(address, sizeof(NvU32), &offset);
    if (aperture != nullptr && offset == aperture->read32CacheOffset)
    {
        HeapFree(GetProcessHeap(), 0, aperture->read32Cache);
        aperture->read32Cache = nullptr;
        aperture->read32CacheOffset = 0;
        aperture->read32CacheCount = 0;
    }
    ReleaseSRWLockExclusive(&apertureLock);
}