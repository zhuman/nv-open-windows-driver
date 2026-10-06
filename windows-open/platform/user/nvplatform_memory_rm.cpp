#include "nvplatform_memory_user.h"
#include "nvplatform_mmio_rm.h"

#include <windows.h>

namespace {

constexpr NvU64 PageSize = 4096;

struct PageAllocation
{
    NV_PLATFORM_DMA_PROVIDER provider;
    void* providerAllocation;
    void* address;
    NvU64 logicalAddress;
    NvU64 size;
};

struct Section
{
    HANDLE handle;
    NvU64 maximumSize;
};

SRWLOCK providerLock = SRWLOCK_INIT;
NV_PLATFORM_DMA_PROVIDER dmaProvider{};
bool providerRegistered;

NvU32 PageCount(NvU64 size) noexcept
{
    return static_cast<NvU32>((size + PageSize - 1) / PageSize);
}

} // namespace

extern "C" NV_STATUS NvPlatformRegisterDmaProvider(
    const NV_PLATFORM_DMA_PROVIDER* provider,
    void** registration) noexcept
{
    if (provider == nullptr || provider->allocate == nullptr ||
        provider->free == nullptr || registration == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *registration = nullptr;

    AcquireSRWLockExclusive(&providerLock);
    NV_STATUS status = NV_ERR_STATE_IN_USE;
    if (!providerRegistered)
    {
        dmaProvider = *provider;
        providerRegistered = true;
        *registration = &dmaProvider;
        status = NV_OK;
    }
    ReleaseSRWLockExclusive(&providerLock);
    return status;
}

extern "C" void NvPlatformUnregisterDmaProvider(void* registration) noexcept
{
    AcquireSRWLockExclusive(&providerLock);
    if (registration == &dmaProvider)
    {
        ZeroMemory(&dmaProvider, sizeof(dmaProvider));
        providerRegistered = false;
    }
    ReleaseSRWLockExclusive(&providerLock);
}

extern "C" NV_STATUS NvPlatformRmAllocatePages(
    NvU64 size,
    NvU64 alignment,
    NvBool contiguous,
    NvBool,
    NvBool belowFourGigabytes,
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType,
    void** allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity,
    NvU32* physicalAddressCount)
{
    return NvPlatformRmAllocatePagesWithPurpose(
        size, alignment, contiguous, NV_FALSE, belowFourGigabytes, cacheType,
        NV_PLATFORM_RM_MEMORY_PURPOSE_DEFAULT, allocation, physicalAddresses,
        physicalAddressCapacity, physicalAddressCount);
}

extern "C" NV_STATUS NvPlatformRmAllocatePagesWithPurpose(
    NvU64 size,
    NvU64 alignment,
    NvBool contiguous,
    NvBool,
    NvBool belowFourGigabytes,
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType,
    NV_PLATFORM_RM_MEMORY_PURPOSE purpose,
    void** allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity,
    NvU32* physicalAddressCount)
{
    if (size == 0 || size > MAXSIZE_T || allocation == nullptr ||
        physicalAddresses == nullptr || physicalAddressCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (alignment == 0)
    {
        alignment = PageSize;
    }
    if ((alignment & (alignment - 1)) != 0 || alignment > MAXDWORD)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (size > static_cast<NvU64>(NV_U32_MAX) * PageSize)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    const NvU32 pageCount = PageCount(size);
    const NvU32 addressCount = contiguous ? 1 : pageCount;
    if (addressCount > physicalAddressCapacity)
    {
        return NV_ERR_INVALID_LIMIT;
    }
    *allocation = nullptr;
    *physicalAddressCount = 0;

    auto* pageAllocation = static_cast<PageAllocation*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PageAllocation)));
    if (pageAllocation == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    AcquireSRWLockShared(&providerLock);
    if (!providerRegistered)
    {
        ReleaseSRWLockShared(&providerLock);
        HeapFree(GetProcessHeap(), 0, pageAllocation);
        return NV_ERR_INVALID_STATE;
    }
    pageAllocation->provider = dmaProvider;
    const NV_STATUS status = pageAllocation->provider.allocate(
        pageAllocation->provider.context,
        size,
        alignment,
        cacheType,
        purpose,
        &pageAllocation->providerAllocation,
        &pageAllocation->address,
        &pageAllocation->logicalAddress);
    ReleaseSRWLockShared(&providerLock);
    if (status != NV_OK)
    {
        HeapFree(GetProcessHeap(), 0, pageAllocation);
        return status;
    }
    if (belowFourGigabytes &&
        (pageAllocation->logicalAddress > MAXDWORD ||
         size - 1 > MAXDWORD - pageAllocation->logicalAddress))
    {
        pageAllocation->provider.free(
            pageAllocation->provider.context,
            pageAllocation->providerAllocation);
        HeapFree(GetProcessHeap(), 0, pageAllocation);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    pageAllocation->size = size;
    for (NvU32 index = 0; index < addressCount; ++index)
    {
        physicalAddresses[index] = pageAllocation->logicalAddress +
            static_cast<NvU64>(index) * PageSize;
    }
    *physicalAddressCount = addressCount;
    *allocation = pageAllocation;
    return NV_OK;
}

extern "C" void NvPlatformRmFreePages(void* allocation)
{
    auto* pageAllocation = static_cast<PageAllocation*>(allocation);
    if (pageAllocation == nullptr)
    {
        return;
    }
    pageAllocation->provider.free(
        pageAllocation->provider.context,
        pageAllocation->providerAllocation);
    SecureZeroMemory(pageAllocation, sizeof(*pageAllocation));
    HeapFree(GetProcessHeap(), 0, pageAllocation);
}

extern "C" NV_STATUS NvPlatformRmLockPages(
    void* allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity)
{
    auto* pageAllocation = static_cast<PageAllocation*>(allocation);
    if (pageAllocation == nullptr || physicalAddresses == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    const NvU32 pageCount = PageCount(pageAllocation->size);
    if (pageCount > physicalAddressCapacity)
    {
        return NV_ERR_INVALID_LIMIT;
    }
    for (NvU32 index = 0; index < pageCount; ++index)
    {
        physicalAddresses[index] = pageAllocation->logicalAddress +
            static_cast<NvU64>(index) * PageSize;
    }
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmUnlockPages(void* allocation)
{
    return allocation != nullptr ? NV_OK : NV_ERR_INVALID_ARGUMENT;
}

extern "C" NV_STATUS NvPlatformRmGetPageCount(
    const void* allocation,
    NvU32* pageCount)
{
    if (allocation == nullptr || pageCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *pageCount = PageCount(static_cast<const PageAllocation*>(allocation)->size);
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetPageTokens(
    const void* allocation,
    NvU64* pageTokens,
    NvU32* pageCount)
{
    if (allocation == nullptr || pageTokens == nullptr || pageCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    const auto* pageAllocation = static_cast<const PageAllocation*>(allocation);
    const NvU32 available = PageCount(pageAllocation->size);
    const NvU32 copied = *pageCount < available ? *pageCount : available;
    for (NvU32 index = 0; index < copied; ++index)
    {
        pageTokens[index] = (pageAllocation->logicalAddress / PageSize) + index;
    }
    *pageCount = copied;
    return NV_OK;
}

extern "C" NvU64 NvPlatformRmPageTokenToPhysicalAddress(
    const void* pageTokens,
    NvU32 pageIndex)
{
    return pageTokens != nullptr
        ? static_cast<const NvU64*>(pageTokens)[pageIndex] * PageSize
        : NV_U64_MAX;
}

extern "C" NV_STATUS NvPlatformRmMapPages(
    void* allocation,
    NvU64 offset,
    NvU64 length,
    NvBool,
    NvBool,
    void** address,
    void** mapping)
{
    auto* pageAllocation = static_cast<PageAllocation*>(allocation);
    if (pageAllocation == nullptr || address == nullptr || mapping == nullptr ||
        length == 0 || offset > pageAllocation->size ||
        length > pageAllocation->size - offset)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *address = static_cast<NvU8*>(pageAllocation->address) +
        static_cast<SIZE_T>(offset);
    *mapping = nullptr;
    return NV_OK;
}

extern "C" void NvPlatformRmUnmapPages(NvBool, void*)
{
}

extern "C" NV_STATUS NvPlatformRmCreateSection(
    NvU64 maximumSize,
    void** section)
{
    if (maximumSize == 0 || maximumSize > MAXLONGLONG || section == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *section = nullptr;
    auto* value = static_cast<Section*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Section)));
    if (value == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }
    value->handle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE | SEC_RESERVE,
        static_cast<DWORD>(maximumSize >> 32),
        static_cast<DWORD>(maximumSize),
        nullptr);
    if (value->handle == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, value);
        return NV_ERR_OPERATING_SYSTEM;
    }
    value->maximumSize = maximumSize;
    *section = value;
    return NV_OK;
}

extern "C" void NvPlatformRmDestroySection(void* section)
{
    auto* value = static_cast<Section*>(section);
    if (value != nullptr)
    {
        CloseHandle(value->handle);
        HeapFree(GetProcessHeap(), 0, value);
    }
}

extern "C" NV_STATUS NvPlatformRmMapSectionView(
    void* section,
    NvU64 offset,
    NvU64 length,
    void** address)
{
    auto* value = static_cast<Section*>(section);
    if (value == nullptr || address == nullptr || length == 0 ||
        length > SIZE_MAX || offset > value->maximumSize ||
        length > value->maximumSize - offset)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *address = MapViewOfFile(
        value->handle,
        FILE_MAP_READ | FILE_MAP_WRITE,
        static_cast<DWORD>(offset >> 32),
        static_cast<DWORD>(offset),
        static_cast<SIZE_T>(length));
    if (*address == nullptr)
    {
        return NV_ERR_OPERATING_SYSTEM;
    }
    if (VirtualAlloc(*address, static_cast<SIZE_T>(length),
            MEM_COMMIT, PAGE_READWRITE) == nullptr)
    {
        UnmapViewOfFile(*address);
        *address = nullptr;
        return NV_ERR_NO_MEMORY;
    }
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmUnmapSectionView(void* address)
{
    if (address == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return UnmapViewOfFile(address) ? NV_OK : NV_ERR_OPERATING_SYSTEM;
}

extern "C" NV_STATUS NvPlatformRmLockSystemAddress(
    void*,
    NvU64,
    void** lockedRange,
    NvU64*,
    NvU32,
    NvU32* physicalAddressCount)
{
    if (lockedRange == nullptr || physicalAddressCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *lockedRange = nullptr;
    *physicalAddressCount = 0;
    return NV_ERR_NOT_SUPPORTED;
}

extern "C" void NvPlatformRmUnlockSystemAddress(void*)
{
}

extern "C" NvU64 NvPlatformRmGetMaximumUserAddress(void)
{
    SYSTEM_INFO information{};
    GetNativeSystemInfo(&information);
    return reinterpret_cast<ULONG_PTR>(information.lpMaximumApplicationAddress);
}