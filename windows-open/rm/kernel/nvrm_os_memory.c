#include "nvplatform_common.h"
#include "nvplatform_memory_rm.h"
#include "nvplatform_mmio_rm.h"
#include "g_os_nvoc.h"
#include "g_mem_desc_nvoc.h"
#include "kernel/os/nv_memory_type.h"

void osInternalReserveAllocCallback(
    NvU64 offset,
    NvU64 size,
    NvU32 gpuId)
{
    // Windows has no external observer for RM-internal framebuffer ranges.
    (void)offset;
    (void)size;
    (void)gpuId;
}

void osInternalReserveFreeCallback(NvU64 offset, NvU32 gpuId)
{
    (void)offset;
    (void)gpuId;
}

NV_STATUS osAllocPagesInternal(MEMORY_DESCRIPTOR* pMemDesc)
{
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType;
    NvU32 physicalAddressCount = 0;
    NvU32 physicalAddressCapacity;
    NV_STATUS status;

    if (pMemDesc == NULL || pMemDesc->Size == 0 ||
        pMemDesc->_addressSpace != ADDR_SYSMEM)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    switch (pMemDesc->_cpuCacheAttrib)
    {
        case NV_MEMORY_CACHED:
        case NV_MEMORY_WRITEBACK:
        case NV_MEMORY_DEFAULT:
            cacheType = NV_PLATFORM_RM_MEMORY_CACHED;
            break;
        case NV_MEMORY_WRITECOMBINED:
            cacheType = NV_PLATFORM_RM_MEMORY_WRITE_COMBINED;
            break;
        default:
            cacheType = NV_PLATFORM_RM_MEMORY_UNCACHED;
            break;
    }

    physicalAddressCapacity = (NvU32)pMemDesc->pageArraySize;
    status = NvPlatformRmAllocatePages(
        pMemDesc->Size,
        pMemDesc->Alignment,
        (pMemDesc->_flags & MEMDESC_FLAGS_PHYSICALLY_CONTIGUOUS) != 0,
        (pMemDesc->_flags & MEMDESC_FLAGS_PAGED_SYSMEM) != 0,
        (pMemDesc->_flags & MEMDESC_FLAGS_ALLOC_32BIT_ADDRESSABLE) != 0,
        cacheType,
        &pMemDesc->_pMemData,
        pMemDesc->_pteArray,
        physicalAddressCapacity,
        &physicalAddressCount);
    if (status != NV_OK)
    {
        pMemDesc->_pMemData = NULL;
        return status;
    }

    pMemDesc->ActualSize = NV_ALIGN_UP(pMemDesc->Size, osGetPageSize());
    pMemDesc->PageCount = pMemDesc->ActualSize / RM_PAGE_SIZE;
    return NV_OK;
}

void osFreePagesInternal(MEMORY_DESCRIPTOR* pMemDesc)
{
    if (pMemDesc != NULL)
    {
        NvPlatformRmFreePages(pMemDesc->_pMemData);
        pMemDesc->_pMemData = NULL;
    }
}

NV_STATUS osReserveCpuAddressSpaceUpperBound(
    void** ppSectionHandle,
    NvU64 maxSectionSize)
{
    return NvPlatformRmCreateSection(maxSectionSize, ppSectionHandle);
}

void osReleaseCpuAddressSpaceUpperBound(void* pSectionHandle)
{
    NvPlatformRmDestroySection(pSectionHandle);
}

NV_STATUS osMapViewToSection(
    OS_GPU_INFO* pOsGpuInfo,
    void* pSectionHandle,
    void** ppAddress,
    NvU64 actualSize,
    NvU64 sectionOffset,
    NvBool bIommuEnabled)
{
    (void)pOsGpuInfo;
    if (bIommuEnabled)
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    return NvPlatformRmMapSectionView(
        pSectionHandle,
        sectionOffset,
        actualSize,
        ppAddress);
}

NV_STATUS osUnmapViewFromSection(
    OS_GPU_INFO* pOsGpuInfo,
    void* pAddress,
    NvBool bIommuEnabled)
{
    (void)pOsGpuInfo;
    if (bIommuEnabled)
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    return NvPlatformRmUnmapSectionView(pAddress);
}

NV_STATUS osLockMem(MEMORY_DESCRIPTOR* pMemDesc)
{
    if (pMemDesc == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NvPlatformRmLockPages(
        pMemDesc->_pMemData,
        pMemDesc->_pteArray,
        (NvU32)pMemDesc->pageArraySize);
}

NV_STATUS osUnlockMem(MEMORY_DESCRIPTOR* pMemDesc)
{
    if (pMemDesc == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NvPlatformRmUnlockPages(pMemDesc->_pMemData);
}

NV_STATUS osGetNumMemoryPages(
    MEMORY_DESCRIPTOR* pMemDesc,
    NvU32* pNumPages)
{
    if (pMemDesc == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NvPlatformRmGetPageCount(pMemDesc->_pMemData, pNumPages);
}

NV_STATUS osGetMemoryPages(
    MEMORY_DESCRIPTOR* pMemDesc,
    void* pPages,
    NvU32* pNumPages)
{
    if (pMemDesc == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NvPlatformRmGetPageTokens(
        pMemDesc->_pMemData,
        (NvU64*)pPages,
        pNumPages);
}

RmPhysAddr osPageArrayGetPhysAddr(
    OS_GPU_INFO* pOsGpuInfo,
    void* pPageData,
    NvU32 pageIndex)
{
    (void)pOsGpuInfo;
    return NvPlatformRmPageTokenToPhysicalAddress(pPageData, pageIndex);
}

NV_STATUS osMapSystemMemory(
    MEMORY_DESCRIPTOR* pMemDesc,
    NvU64 offset,
    NvU64 length,
    NvBool kernel,
    NvU32 protect,
    NvP64* ppAddress,
    NvP64* ppPrivate)
{
    NvU64 rootOffset = 0;
    void* address = NULL;
    void* mapping = NULL;
    NV_STATUS status;

    if (pMemDesc == NULL || ppAddress == NULL || ppPrivate == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    pMemDesc = memdescGetRootMemDesc(pMemDesc, &rootOffset);
    if (offset > NV_U64_MAX - rootOffset)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    offset += rootOffset;

    status = NvPlatformRmMapPages(
        pMemDesc->_pMemData,
        offset,
        length,
        kernel,
        (protect & NV_PROTECT_WRITEABLE) != 0,
        &address,
        &mapping);
    *ppAddress = NV_PTR_TO_NvP64(address);
    *ppPrivate = NV_PTR_TO_NvP64(mapping);
    return status;
}

void osUnmapSystemMemory(
    MEMORY_DESCRIPTOR* pMemDesc,
    NvBool kernel,
    NvP64 pAddress,
    NvP64 pPrivate)
{
    (void)pMemDesc;
    (void)pAddress;
    NvPlatformRmUnmapPages(kernel, NvP64_VALUE(pPrivate));
}

NvU64 osGetPageSize(void)
{
    return NvPlatformGetPageSize();
}

NvU8 osGetPageShift(void)
{
    NvU64 pageSize = NvPlatformGetPageSize();
    NvU8 pageShift = 0;

    while (pageSize > 1)
    {
        pageSize >>= 1;
        ++pageShift;
    }

    return pageShift;
}

NvU64 osGetSupportedSysmemPageSizeMask(void)
{
    return NvPlatformGetPageSize();
}

NvU64 osGetMaxUserVa(void)
{
    return NvPlatformRmGetMaximumUserAddress();
}