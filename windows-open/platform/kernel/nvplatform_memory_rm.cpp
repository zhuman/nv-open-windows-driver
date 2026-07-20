#include "nvplatform_memory_rm.h"
#include "nvplatform_file_rm.h"

#include <ntifs.h>

namespace {

constexpr ULONG MemoryAllocationTag = 'mRvN';
constexpr ULONG SectionAllocationTag = 'sRvN';
constexpr ULONG LockedRangeAllocationTag = 'lRvN';
constexpr NvU64 SectionViewAlignment = 64 * 1024;
constexpr ULONG MaximumMdlChunkSize = 16 * 1024 * 1024;

struct RmPageAllocation
{
    PMDL mdl;
    void* pageableBase;
    void* contiguousBase;
    SIZE_T contiguousSize;
    SIZE_T size;
    MEMORY_CACHING_TYPE cacheType;
};

struct RmPageMapping
{
    PMDL mdl;
    void* baseAddress;
    PEPROCESS process;
};

struct RmSection
{
    void* file;
    void* sectionObject;
    NvU64 maximumSize;
};

struct RmLockedRange
{
    PMDL mdl;
    RmLockedRange* next;
};

void UnlockSystemAddress(RmLockedRange* lockedRange)
{
    while (lockedRange != nullptr)
    {
        RmLockedRange* next = lockedRange->next;
        MmUnlockPages(lockedRange->mdl);
        IoFreeMdl(lockedRange->mdl);
        ExFreePoolWithTag(lockedRange, LockedRangeAllocationTag);
        lockedRange = next;
    }
}

MEMORY_CACHING_TYPE ConvertCacheType(
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE cacheType)
{
    switch (cacheType)
    {
        case NV_PLATFORM_RM_MEMORY_CACHED:
            return MmCached;
        case NV_PLATFORM_RM_MEMORY_WRITE_COMBINED:
            return MmWriteCombined;
        default:
            return MmNonCached;
    }
}

} // namespace

extern "C" NV_STATUS NvPlatformCopyFromUser(
    const void* userAddress,
    void* kernelAddress,
    NvLength length)
{
    __try
    {
        ProbeForRead(userAddress, length, 1);
        RtlCopyMemory(kernelAddress, userAddress, length);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    return NV_OK;
}

extern "C" NV_STATUS NvPlatformCopyToUser(
    const void* kernelAddress,
    void* userAddress,
    NvLength length)
{
    __try
    {
        ProbeForWrite(userAddress, length, 1);
        RtlCopyMemory(userAddress, kernelAddress, length);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmAllocatePages(
    NvU64 size,
    NvU64 alignment,
    NvBool contiguous,
    NvBool pageable,
    NvBool belowFourGigabytes,
    NV_PLATFORM_RM_MEMORY_CACHE_TYPE requestedCacheType,
    void** allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity,
    NvU32* physicalAddressCount)
{
    if (size == 0 || size > MAXULONG || allocation == nullptr ||
        physicalAddresses == nullptr || physicalAddressCount == nullptr ||
        KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const SIZE_T pageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(nullptr, size);
    const SIZE_T outputAddressCount = pageable ? 0 : (contiguous ? 1 : pageCount);
    if (outputAddressCount > physicalAddressCapacity || pageCount > MAXULONG)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    if (alignment == 0)
    {
        alignment = PAGE_SIZE;
    }
    if ((alignment & (alignment - 1)) != 0 || alignment > MAXULONG)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (pageable &&
        (contiguous || belowFourGigabytes || alignment > PAGE_SIZE ||
         requestedCacheType != NV_PLATFORM_RM_MEMORY_CACHED))
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    auto* pageAllocation = static_cast<RmPageAllocation*>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(RmPageAllocation), MemoryAllocationTag));
    if (pageAllocation == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }
    RtlZeroMemory(pageAllocation, sizeof(*pageAllocation));

    const MEMORY_CACHING_TYPE cacheType = ConvertCacheType(requestedCacheType);
    PHYSICAL_ADDRESS lowAddress{};
    PHYSICAL_ADDRESS highAddress{};
    highAddress.QuadPart = belowFourGigabytes
        ? MAXULONG
        : MAXLONGLONG;

    if (pageable)
    {
        pageAllocation->pageableBase = ExAllocatePool2(
            POOL_FLAG_PAGED,
            static_cast<SIZE_T>(size),
            MemoryAllocationTag);
        if (pageAllocation->pageableBase == nullptr)
        {
            ExFreePoolWithTag(pageAllocation, MemoryAllocationTag);
            return NV_ERR_NO_MEMORY;
        }
    }
    else if (contiguous || alignment > PAGE_SIZE)
    {
        if (size > MAXULONG - alignment)
        {
            ExFreePoolWithTag(pageAllocation, MemoryAllocationTag);
            return NV_ERR_INVALID_LIMIT;
        }

        const SIZE_T allocationSize = static_cast<SIZE_T>(size + alignment);
        void* contiguousBase = MmAllocateContiguousMemorySpecifyCache(
            allocationSize,
            lowAddress,
            highAddress,
            PHYSICAL_ADDRESS{},
            cacheType);
        if (contiguousBase == nullptr)
        {
            ExFreePoolWithTag(pageAllocation, MemoryAllocationTag);
            return NV_ERR_NO_MEMORY;
        }

        const NvU64 basePhysical = MmGetPhysicalAddress(contiguousBase).QuadPart;
        const SIZE_T adjustment = static_cast<SIZE_T>(
            (alignment - (basePhysical & (alignment - 1))) & (alignment - 1));
        void* alignedAddress = static_cast<NvU8*>(contiguousBase) + adjustment;
        PMDL mdl = IoAllocateMdl(
            alignedAddress,
            static_cast<ULONG>(size),
            FALSE,
            FALSE,
            nullptr);
        if (mdl == nullptr)
        {
            MmFreeContiguousMemorySpecifyCache(contiguousBase, allocationSize, cacheType);
            ExFreePoolWithTag(pageAllocation, MemoryAllocationTag);
            return NV_ERR_NO_MEMORY;
        }
        MmBuildMdlForNonPagedPool(mdl);

        pageAllocation->mdl = mdl;
        pageAllocation->contiguousBase = contiguousBase;
        pageAllocation->contiguousSize = allocationSize;
    }
    else
    {
        pageAllocation->mdl = MmAllocatePagesForMdlEx(
            lowAddress,
            highAddress,
            PHYSICAL_ADDRESS{},
            static_cast<SIZE_T>(size),
            cacheType,
            MM_ALLOCATE_FULLY_REQUIRED);
        if (pageAllocation->mdl == nullptr)
        {
            ExFreePoolWithTag(pageAllocation, MemoryAllocationTag);
            return NV_ERR_NO_MEMORY;
        }
    }

    pageAllocation->size = static_cast<SIZE_T>(size);
    pageAllocation->cacheType = cacheType;

    *physicalAddressCount = static_cast<NvU32>(outputAddressCount);
    if (outputAddressCount != 0)
    {
        const PPFN_NUMBER pageFrameNumbers = MmGetMdlPfnArray(pageAllocation->mdl);
        for (SIZE_T index = 0; index < outputAddressCount; ++index)
        {
            physicalAddresses[index] =
                static_cast<NvU64>(pageFrameNumbers[index]) << PAGE_SHIFT;
        }
    }

    *allocation = pageAllocation;
    return NV_OK;
}

extern "C" void NvPlatformRmFreePages(void* allocation)
{
    auto* pageAllocation = static_cast<RmPageAllocation*>(allocation);
    if (pageAllocation == nullptr)
    {
        return;
    }

    if (pageAllocation->pageableBase != nullptr)
    {
        if (pageAllocation->mdl != nullptr)
        {
            MmUnlockPages(pageAllocation->mdl);
            IoFreeMdl(pageAllocation->mdl);
        }
        ExFreePoolWithTag(pageAllocation->pageableBase, MemoryAllocationTag);
    }
    else if (pageAllocation->contiguousBase != nullptr)
    {
        IoFreeMdl(pageAllocation->mdl);
        MmFreeContiguousMemorySpecifyCache(
            pageAllocation->contiguousBase,
            pageAllocation->contiguousSize,
            pageAllocation->cacheType);
    }
    else
    {
        MmFreePagesFromMdl(pageAllocation->mdl);
        ExFreePool(pageAllocation->mdl);
    }
    ExFreePoolWithTag(pageAllocation, MemoryAllocationTag);
}

extern "C" NV_STATUS NvPlatformRmLockPages(
    void* allocation,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity)
{
    auto* pageAllocation = static_cast<RmPageAllocation*>(allocation);
    if (pageAllocation == nullptr || pageAllocation->pageableBase == nullptr ||
        physicalAddresses == nullptr || pageAllocation->mdl != nullptr ||
        KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const SIZE_T pageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(
        pageAllocation->pageableBase,
        pageAllocation->size);
    if (pageCount > physicalAddressCapacity)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    PMDL mdl = IoAllocateMdl(
        pageAllocation->pageableBase,
        static_cast<ULONG>(pageAllocation->size),
        FALSE,
        FALSE,
        nullptr);
    if (mdl == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    __try
    {
        MmProbeAndLockPages(mdl, KernelMode, IoModifyAccess);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        IoFreeMdl(mdl);
        return NV_ERR_OPERATING_SYSTEM;
    }

    const PPFN_NUMBER pageFrameNumbers = MmGetMdlPfnArray(mdl);
    for (SIZE_T index = 0; index < pageCount; ++index)
    {
        physicalAddresses[index] =
            static_cast<NvU64>(pageFrameNumbers[index]) << PAGE_SHIFT;
    }
    pageAllocation->mdl = mdl;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmUnlockPages(void* allocation)
{
    auto* pageAllocation = static_cast<RmPageAllocation*>(allocation);
    if (pageAllocation == nullptr || pageAllocation->pageableBase == nullptr ||
        pageAllocation->mdl == nullptr || KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    MmUnlockPages(pageAllocation->mdl);
    IoFreeMdl(pageAllocation->mdl);
    pageAllocation->mdl = nullptr;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetPageCount(
    const void* allocation,
    NvU32* pageCount)
{
    const auto* pageAllocation = static_cast<const RmPageAllocation*>(allocation);
    if (pageAllocation == nullptr || pageCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *pageCount = static_cast<NvU32>(
        ADDRESS_AND_SIZE_TO_SPAN_PAGES(nullptr, pageAllocation->size));
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGetPageTokens(
    const void* allocation,
    NvU64* pageTokens,
    NvU32* pageCount)
{
    const auto* pageAllocation = static_cast<const RmPageAllocation*>(allocation);
    if (pageAllocation == nullptr || pageTokens == nullptr || pageCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (pageAllocation->mdl == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    const NvU32 availablePages = static_cast<NvU32>(
        ADDRESS_AND_SIZE_TO_SPAN_PAGES(nullptr, pageAllocation->size));
    const NvU32 copiedPages = *pageCount < availablePages
        ? *pageCount
        : availablePages;
    const PPFN_NUMBER pageFrameNumbers = MmGetMdlPfnArray(pageAllocation->mdl);
    for (NvU32 index = 0; index < copiedPages; ++index)
    {
        pageTokens[index] = pageFrameNumbers[index];
    }
    *pageCount = copiedPages;
    return NV_OK;
}

extern "C" NvU64 NvPlatformRmPageTokenToPhysicalAddress(
    const void* pageTokens,
    NvU32 pageIndex)
{
    if (pageTokens == nullptr)
    {
        return NV_U64_MAX;
    }

    return static_cast<const NvU64*>(pageTokens)[pageIndex] << PAGE_SHIFT;
}

extern "C" NV_STATUS NvPlatformRmMapPages(
    void* allocation,
    NvU64 offset,
    NvU64 length,
    NvBool kernelMapping,
    NvBool writable,
    void** address,
    void** mapping)
{
    auto* pageAllocation = static_cast<RmPageAllocation*>(allocation);
    if (pageAllocation == nullptr || address == nullptr || mapping == nullptr ||
        length == 0 || offset > pageAllocation->size ||
        length > pageAllocation->size - offset || length > MAXULONG ||
        KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *address = nullptr;
    *mapping = nullptr;
    if (kernelMapping && pageAllocation->pageableBase != nullptr)
    {
        *address = static_cast<NvU8*>(pageAllocation->pageableBase) +
            static_cast<SIZE_T>(offset);
        return NV_OK;
    }
    if (pageAllocation->mdl == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }
    if (kernelMapping && pageAllocation->contiguousBase != nullptr)
    {
        *address = static_cast<NvU8*>(MmGetMdlVirtualAddress(pageAllocation->mdl)) +
            static_cast<SIZE_T>(offset);
        return NV_OK;
    }

    const SIZE_T alignedOffset = static_cast<SIZE_T>(offset) & ~(PAGE_SIZE - 1);
    const SIZE_T pageOffset = static_cast<SIZE_T>(offset) & (PAGE_SIZE - 1);
    if (length > MAXULONG - pageOffset)
    {
        return NV_ERR_INVALID_LIMIT;
    }
    const SIZE_T mappedLength = static_cast<SIZE_T>(length) + pageOffset;
    void* sourceAddress = static_cast<NvU8*>(
        MmGetMdlVirtualAddress(pageAllocation->mdl)) + alignedOffset;
    PMDL partialMdl = IoAllocateMdl(
        sourceAddress,
        static_cast<ULONG>(mappedLength),
        FALSE,
        FALSE,
        nullptr);
    if (partialMdl == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }
    IoBuildPartialMdl(
        pageAllocation->mdl,
        partialMdl,
        sourceAddress,
        static_cast<ULONG>(mappedLength));

    auto* pageMapping = static_cast<RmPageMapping*>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(RmPageMapping), MemoryAllocationTag));
    if (pageMapping == nullptr)
    {
        IoFreeMdl(partialMdl);
        return NV_ERR_NO_MEMORY;
    }

    ULONG priority = NormalPagePriority | MdlMappingNoExecute;
    if (!writable)
    {
        priority |= MdlMappingNoWrite;
    }

    void* baseAddress = nullptr;
    const KPROCESSOR_MODE accessMode = kernelMapping
        ? static_cast<KPROCESSOR_MODE>(KernelMode)
        : static_cast<KPROCESSOR_MODE>(UserMode);
    __try
    {
        baseAddress = MmMapLockedPagesSpecifyCache(
            partialMdl,
            accessMode,
            pageAllocation->cacheType,
            nullptr,
            FALSE,
            priority);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        baseAddress = nullptr;
    }
    if (baseAddress == nullptr)
    {
        ExFreePoolWithTag(pageMapping, MemoryAllocationTag);
        IoFreeMdl(partialMdl);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    pageMapping->mdl = partialMdl;
    pageMapping->baseAddress = baseAddress;
    pageMapping->process = nullptr;
    if (!kernelMapping)
    {
        pageMapping->process = PsGetCurrentProcess();
        ObReferenceObject(pageMapping->process);
    }
    *address = static_cast<NvU8*>(baseAddress) + pageOffset;
    *mapping = pageMapping;
    return NV_OK;
}

extern "C" void NvPlatformRmUnmapPages(
    NvBool kernelMapping,
    void* mapping)
{
    auto* pageMapping = static_cast<RmPageMapping*>(mapping);
    if (pageMapping == nullptr)
    {
        return;
    }

    UNREFERENCED_PARAMETER(kernelMapping);
    KAPC_STATE apcState{};
    const bool attach = pageMapping->process != nullptr &&
        pageMapping->process != PsGetCurrentProcess();
    if (attach)
    {
        KeStackAttachProcess(pageMapping->process, &apcState);
    }
    MmUnmapLockedPages(pageMapping->baseAddress, pageMapping->mdl);
    if (attach)
    {
        KeUnstackDetachProcess(&apcState);
    }
    if (pageMapping->process != nullptr)
    {
        ObDereferenceObject(pageMapping->process);
    }
    IoFreeMdl(pageMapping->mdl);
    ExFreePoolWithTag(pageMapping, MemoryAllocationTag);
}

extern "C" NV_STATUS NvPlatformRmCreateSection(
    NvU64 maximumSize,
    void** section)
{
    if (maximumSize == 0 || maximumSize > MAXLONGLONG || section == nullptr ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *section = nullptr;
    auto* rmSection = static_cast<RmSection*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(RmSection),
        SectionAllocationTag));
    if (rmSection == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }
    RtlZeroMemory(rmSection, sizeof(*rmSection));

    NV_STATUS rmStatus = NvPlatformRmOpenTemporaryFile(&rmSection->file);
    if (rmStatus != NV_OK)
    {
        ExFreePoolWithTag(rmSection, SectionAllocationTag);
        return rmStatus;
    }

    IO_STATUS_BLOCK ioStatus{};
    FILE_END_OF_FILE_INFORMATION endOfFile{};
    endOfFile.EndOfFile.QuadPart = static_cast<LONGLONG>(maximumSize);
    NTSTATUS status = ZwSetInformationFile(
        static_cast<HANDLE>(rmSection->file),
        &ioStatus,
        &endOfFile,
        sizeof(endOfFile),
        FileEndOfFileInformation);

    HANDLE sectionHandle = nullptr;
    if (NT_SUCCESS(status))
    {
        OBJECT_ATTRIBUTES attributes{};
        InitializeObjectAttributes(
            &attributes,
            nullptr,
            OBJ_KERNEL_HANDLE,
            nullptr,
            nullptr);
        LARGE_INTEGER sectionSize{};
        sectionSize.QuadPart = static_cast<LONGLONG>(maximumSize);
        status = ZwCreateSection(
            &sectionHandle,
            SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY,
            &attributes,
            &sectionSize,
            PAGE_READWRITE,
            SEC_COMMIT,
            static_cast<HANDLE>(rmSection->file));
    }
    if (NT_SUCCESS(status))
    {
        status = ObReferenceObjectByHandle(
            sectionHandle,
            SECTION_MAP_READ | SECTION_MAP_WRITE,
            nullptr,
            KernelMode,
            &rmSection->sectionObject,
            nullptr);
    }
    if (sectionHandle != nullptr)
    {
        ZwClose(sectionHandle);
    }
    if (!NT_SUCCESS(status))
    {
        NvPlatformRmCloseFile(rmSection->file);
        ExFreePoolWithTag(rmSection, SectionAllocationTag);
        return NV_ERR_OPERATING_SYSTEM;
    }

    rmSection->maximumSize = maximumSize;
    *section = rmSection;
    return NV_OK;
}

extern "C" void NvPlatformRmDestroySection(void* section)
{
    auto* rmSection = static_cast<RmSection*>(section);
    if (rmSection == nullptr)
    {
        return;
    }

    ObDereferenceObject(rmSection->sectionObject);
    NvPlatformRmCloseFile(rmSection->file);
    ExFreePoolWithTag(rmSection, SectionAllocationTag);
}

extern "C" NV_STATUS NvPlatformRmMapSectionView(
    void* section,
    NvU64 offset,
    NvU64 length,
    void** address)
{
    auto* rmSection = static_cast<RmSection*>(section);
    if (rmSection == nullptr || address == nullptr || length == 0 ||
        length > MAXSIZE_T || offset > MAXLONGLONG ||
        (offset & (SectionViewAlignment - 1)) != 0 ||
        offset > rmSection->maximumSize ||
        length > rmSection->maximumSize - offset ||
        KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *address = nullptr;
    SIZE_T viewSize = static_cast<SIZE_T>(length);
    LARGE_INTEGER sectionOffset{};
    sectionOffset.QuadPart = static_cast<LONGLONG>(offset);
    return NT_SUCCESS(MmMapViewInSystemSpaceEx(
        rmSection->sectionObject,
        address,
        &viewSize,
        &sectionOffset,
        0))
        ? NV_OK
        : NV_ERR_INSUFFICIENT_RESOURCES;
}

extern "C" NV_STATUS NvPlatformRmUnmapSectionView(void* address)
{
    if (address == nullptr || KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NT_SUCCESS(MmUnmapViewInSystemSpace(address))
        ? NV_OK
        : NV_ERR_OPERATING_SYSTEM;
}

extern "C" NV_STATUS NvPlatformRmLockSystemAddress(
    void* address,
    NvU64 length,
    void** lockedRange,
    NvU64* physicalAddresses,
    NvU32 physicalAddressCapacity,
    NvU32* physicalAddressCount)
{
    if (address == nullptr || length == 0 || lockedRange == nullptr ||
        physicalAddresses == nullptr || physicalAddressCount == nullptr ||
        (reinterpret_cast<ULONG_PTR>(address) & (PAGE_SIZE - 1)) != 0 ||
        KeGetCurrentIrql() > APC_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (length > NV_U64_MAX - (PAGE_SIZE - 1))
    {
        return NV_ERR_INVALID_LIMIT;
    }
    const NvU64 pageCount = (length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (pageCount > physicalAddressCapacity || pageCount > NV_U32_MAX)
    {
        return NV_ERR_BUFFER_TOO_SMALL;
    }

    *lockedRange = nullptr;
    *physicalAddressCount = 0;
    auto* cursor = static_cast<NvU8*>(address);
    NvU64 remaining = length;
    RmLockedRange* head = nullptr;
    RmLockedRange** tail = &head;

    while (remaining != 0)
    {
        const ULONG chunkSize = static_cast<ULONG>(
            remaining > MaximumMdlChunkSize ? MaximumMdlChunkSize : remaining);
        auto* node = static_cast<RmLockedRange*>(ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            sizeof(RmLockedRange),
            LockedRangeAllocationTag));
        if (node == nullptr)
        {
            UnlockSystemAddress(head);
            return NV_ERR_NO_MEMORY;
        }
        node->next = nullptr;
        node->mdl = IoAllocateMdl(cursor, chunkSize, FALSE, FALSE, nullptr);
        if (node->mdl == nullptr)
        {
            ExFreePoolWithTag(node, LockedRangeAllocationTag);
            UnlockSystemAddress(head);
            return NV_ERR_NO_MEMORY;
        }

        bool locked = false;
        __try
        {
            MmProbeAndLockPages(node->mdl, KernelMode, IoModifyAccess);
            locked = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (!locked)
        {
            IoFreeMdl(node->mdl);
            ExFreePoolWithTag(node, LockedRangeAllocationTag);
            UnlockSystemAddress(head);
            return NV_ERR_INVALID_ADDRESS;
        }

        const NvU32 mdlPageCount = ADDRESS_AND_SIZE_TO_SPAN_PAGES(cursor, chunkSize);
        if (mdlPageCount > physicalAddressCapacity - *physicalAddressCount)
        {
            MmUnlockPages(node->mdl);
            IoFreeMdl(node->mdl);
            ExFreePoolWithTag(node, LockedRangeAllocationTag);
            UnlockSystemAddress(head);
            return NV_ERR_BUFFER_TOO_SMALL;
        }
        const PPFN_NUMBER pageFrameNumbers = MmGetMdlPfnArray(node->mdl);
        for (NvU32 index = 0; index < mdlPageCount; ++index)
        {
            physicalAddresses[*physicalAddressCount + index] =
                static_cast<NvU64>(pageFrameNumbers[index]) << PAGE_SHIFT;
        }
        *physicalAddressCount += mdlPageCount;

        *tail = node;
        tail = &node->next;
        cursor += chunkSize;
        remaining -= chunkSize;
    }

    *lockedRange = head;
    return NV_OK;
}

extern "C" void NvPlatformRmUnlockSystemAddress(void* lockedRange)
{
    UnlockSystemAddress(static_cast<RmLockedRange*>(lockedRange));
}
