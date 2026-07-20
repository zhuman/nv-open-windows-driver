#include "nvplatform_mmio.h"
#include "nvplatform_mmio_rm.h"

#include "kernel/os/nv_memory_type.h"

namespace {

constexpr ULONG RmMappingPoolTag = 'mRvN';

struct RmPhysicalMapping
{
    LIST_ENTRY listEntry;
    NV_PLATFORM_MMIO_MAPPING mapping;
};

EX_PUSH_LOCK rmMappingLock;
LIST_ENTRY rmMappings;
bool mmioInitialized;

bool IsValidRegisterAccess(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset) noexcept
{
    return mapping != nullptr &&
        mapping->address != nullptr &&
        (offset & (sizeof(ULONG) - 1)) == 0 &&
        offset <= mapping->length &&
        mapping->length - offset >= sizeof(ULONG);
}

NTSTATUS MapMmioWithProtection(
    _In_ PHYSICAL_ADDRESS physicalAddress,
    _In_ SIZE_T length,
    _In_ ULONG pageProtection,
    _Out_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept
{
    if (mapping == nullptr || length == 0 || physicalAddress.QuadPart < 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(mapping, sizeof(*mapping));

    constexpr ULONGLONG PageMask = PAGE_SIZE - 1;
    const SIZE_T pageOffset = static_cast<SIZE_T>(
        static_cast<ULONGLONG>(physicalAddress.QuadPart) & PageMask);
    if (length > MAXSIZE_T - pageOffset)
    {
        return STATUS_INTEGER_OVERFLOW;
    }

    const SIZE_T unroundedLength = length + pageOffset;
    if (unroundedLength > MAXSIZE_T - PageMask)
    {
        return STATUS_INTEGER_OVERFLOW;
    }

    const SIZE_T mappingLength = (unroundedLength + PageMask) & ~PageMask;
    PHYSICAL_ADDRESS alignedAddress = {};
    alignedAddress.QuadPart = physicalAddress.QuadPart & ~PageMask;

    void* mappingBase = MmMapIoSpaceEx(
        alignedAddress,
        mappingLength,
        pageProtection);
    if (mappingBase == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mapping->mappingBase = mappingBase;
    mapping->address = static_cast<volatile UCHAR*>(mappingBase) + pageOffset;
    mapping->mappingLength = mappingLength;
    mapping->length = length;
    return STATUS_SUCCESS;
}

NTSTATUS GetPageProtection(
    _In_ NvU32 protect,
    _In_ NvU32 cacheType,
    _Out_ ULONG* pageProtection) noexcept
{
    if (pageProtection == nullptr ||
        (protect & ~NV_PROTECT_READ_WRITE) != 0 || protect == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG value = protect == NV_PROTECT_READABLE
        ? PAGE_READONLY
        : PAGE_READWRITE;
    switch (cacheType)
    {
    case NV_MEMORY_CACHED:
    case NV_MEMORY_WRITEBACK:
        break;
    case NV_MEMORY_UNCACHED:
    case NV_MEMORY_DEFAULT:
    case NV_MEMORY_UNCACHED_WEAK:
        value |= PAGE_NOCACHE;
        break;
    case NV_MEMORY_WRITECOMBINED:
        value |= PAGE_WRITECOMBINE;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    *pageProtection = value;
    return STATUS_SUCCESS;
}

} // namespace

extern "C" NTSTATUS NvPlatformMmioInitialize(void) noexcept
{
    if (mmioInitialized)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    ExInitializePushLock(&rmMappingLock);
    InitializeListHead(&rmMappings);
    mmioInitialized = true;
    return STATUS_SUCCESS;
}

extern "C" void NvPlatformMmioShutdown(void) noexcept
{
    if (!mmioInitialized)
    {
        return;
    }

    for (;;)
    {
        ExAcquirePushLockExclusive(&rmMappingLock);
        if (IsListEmpty(&rmMappings))
        {
            mmioInitialized = false;
            ExReleasePushLockExclusive(&rmMappingLock);
            break;
        }

        PLIST_ENTRY entry = RemoveHeadList(&rmMappings);
        ExReleasePushLockExclusive(&rmMappingLock);

        auto* rmMapping = CONTAINING_RECORD(
            entry,
            RmPhysicalMapping,
            listEntry);
        NvPlatformUnmapMmio(&rmMapping->mapping);
        ExFreePoolWithTag(rmMapping, RmMappingPoolTag);
    }
}

extern "C" NTSTATUS NvPlatformMapMmio(
    _In_ PHYSICAL_ADDRESS physicalAddress,
    _In_ SIZE_T length,
    _Out_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept
{
    return MapMmioWithProtection(
        physicalAddress,
        length,
        PAGE_READWRITE | PAGE_NOCACHE,
        mapping);
}

extern "C" void NvPlatformUnmapMmio(
    _Inout_ NV_PLATFORM_MMIO_MAPPING* mapping) noexcept
{
    if (mapping == nullptr)
    {
        return;
    }

    if (mapping->mappingBase != nullptr)
    {
        MmUnmapIoSpace(mapping->mappingBase, mapping->mappingLength);
    }

    RtlZeroMemory(mapping, sizeof(*mapping));
}

extern "C" NTSTATUS NvPlatformReadMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _Out_ ULONG* value) noexcept
{
    if (value == nullptr || !IsValidRegisterAccess(mapping, offset))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *value = READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG*>(
        mapping->address + offset));
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS NvPlatformWriteMmio32(
    _In_ const NV_PLATFORM_MMIO_MAPPING* mapping,
    _In_ SIZE_T offset,
    _In_ ULONG value) noexcept
{
    if (!IsValidRegisterAccess(mapping, offset))
    {
        return STATUS_INVALID_PARAMETER;
    }

    WRITE_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG*>(mapping->address + offset),
        value);
    return STATUS_SUCCESS;
}

extern "C" NV_STATUS NvPlatformRmMapPhysical(
    NvU64 physicalAddress,
    NvU64 length,
    NvU32 protect,
    NvU32 cacheType,
    void** virtualAddress)
{
    if (!mmioInitialized || virtualAddress == nullptr ||
        length == 0 || length > MAXSIZE_T || physicalAddress > MAXLONGLONG)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *virtualAddress = nullptr;
    ULONG pageProtection = 0;
    if (!NT_SUCCESS(GetPageProtection(protect, cacheType, &pageProtection)))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    auto* rmMapping = static_cast<RmPhysicalMapping*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(RmPhysicalMapping),
        RmMappingPoolTag));
    if (rmMapping == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    RtlZeroMemory(rmMapping, sizeof(*rmMapping));
    PHYSICAL_ADDRESS address = {};
    address.QuadPart = static_cast<LONGLONG>(physicalAddress);
    const NTSTATUS status = MapMmioWithProtection(
        address,
        static_cast<SIZE_T>(length),
        pageProtection,
        &rmMapping->mapping);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(rmMapping, RmMappingPoolTag);
        return status == STATUS_INSUFFICIENT_RESOURCES
            ? NV_ERR_NO_MEMORY
            : NV_ERR_GENERIC;
    }

    ExAcquirePushLockExclusive(&rmMappingLock);
    if (!mmioInitialized)
    {
        ExReleasePushLockExclusive(&rmMappingLock);
        NvPlatformUnmapMmio(&rmMapping->mapping);
        ExFreePoolWithTag(rmMapping, RmMappingPoolTag);
        return NV_ERR_INVALID_STATE;
    }
    InsertTailList(&rmMappings, &rmMapping->listEntry);
    ExReleasePushLockExclusive(&rmMappingLock);

    *virtualAddress = const_cast<UCHAR*>(rmMapping->mapping.address);
    return NV_OK;
}

extern "C" void NvPlatformRmUnmapPhysical(void* virtualAddress)
{
    if (!mmioInitialized || virtualAddress == nullptr)
    {
        return;
    }

    RmPhysicalMapping* found = nullptr;
    ExAcquirePushLockExclusive(&rmMappingLock);
    for (PLIST_ENTRY entry = rmMappings.Flink;
         entry != &rmMappings;
         entry = entry->Flink)
    {
        auto* rmMapping = CONTAINING_RECORD(
            entry,
            RmPhysicalMapping,
            listEntry);
        if (rmMapping->mapping.address == virtualAddress)
        {
            RemoveEntryList(entry);
            found = rmMapping;
            break;
        }
    }
    ExReleasePushLockExclusive(&rmMappingLock);

    NT_ASSERT(found != nullptr);
    if (found != nullptr)
    {
        NvPlatformUnmapMmio(&found->mapping);
        ExFreePoolWithTag(found, RmMappingPoolTag);
    }
}

extern "C" NvU8 NvPlatformRmReadRegister8(const volatile void* address)
{
    return READ_REGISTER_UCHAR(reinterpret_cast<volatile UCHAR*>(
        const_cast<volatile void*>(address)));
}

extern "C" NvU16 NvPlatformRmReadRegister16(const volatile void* address)
{
    return READ_REGISTER_USHORT(reinterpret_cast<volatile USHORT*>(
        const_cast<volatile void*>(address)));
}

extern "C" NvU32 NvPlatformRmReadRegister32(const volatile void* address)
{
    return READ_REGISTER_ULONG(reinterpret_cast<volatile ULONG*>(
        const_cast<volatile void*>(address)));
}

extern "C" void NvPlatformRmWriteRegister8(volatile void* address, NvU8 value)
{
    WRITE_REGISTER_UCHAR(
        reinterpret_cast<volatile UCHAR*>(address),
        value);
}

extern "C" void NvPlatformRmWriteRegister16(volatile void* address, NvU16 value)
{
    WRITE_REGISTER_USHORT(
        reinterpret_cast<volatile USHORT*>(address),
        value);
}

extern "C" void NvPlatformRmWriteRegister32(volatile void* address, NvU32 value)
{
    WRITE_REGISTER_ULONG(
        reinterpret_cast<volatile ULONG*>(address),
        value);
}

extern "C" NvU64 NvPlatformRmGetMaximumUserAddress(void)
{
    return reinterpret_cast<NvU64>(MmHighestUserAddress);
}