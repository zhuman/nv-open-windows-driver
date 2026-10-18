#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <gtest/gtest.h>
extern "C" {
#include <wudfwdm.h>
}

#include "../../platform/kernel/nvplatform_memory_resource.hpp"
#include "../../platform/kernel/nvplatform_mmio_user.h"
#include "../../platform/kernel/nvplatform_mmio_rm.h"
#include "kernel/os/nv_memory_type.h"
#include <array>
#include <atomic>
#include <memory>
#include <new>
#include <thread>

namespace {

CM_PARTIAL_RESOURCE_DESCRIPTOR Memory(ULONGLONG address, ULONG length, USHORT large = 0)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR descriptor = {};
    descriptor.Type = large == 0 ? CmResourceTypeMemory : CmResourceTypeMemoryLarge;
    descriptor.Flags = large | CM_RESOURCE_MEMORY_PREFETCHABLE;
    descriptor.u.Memory.Start.QuadPart = static_cast<LONGLONG>(address);
    descriptor.u.Memory.Length = length;
    return descriptor;
}

TEST(BarResources, DecodesAndResolvesMemoryRanges)
{
    // Large-resource lengths are encoded units, not bytes. Match the raw BAR
    // address to its translated CPU range without losing high address bits.
    using nv::platform::DecodeMemoryResource;
    nv::platform::MemoryResource decoded = {};
    const USHORT encodings[] = { 0, CM_RESOURCE_MEMORY_LARGE_40,
        CM_RESOURCE_MEMORY_LARGE_48, CM_RESOURCE_MEMORY_LARGE_64 };
    const unsigned shifts[] = { 0, 8, 16, 32 };
    for (unsigned encoding = 0; encoding < 4; ++encoding)
    {
        for (ULONG length = 1; length <= 4096; ++length)
        {
            const auto descriptor = Memory(0x2800000000ULL, length, encodings[encoding]);
            EXPECT_TRUE(DecodeMemoryResource(descriptor, &decoded) == STATUS_SUCCESS);
            EXPECT_TRUE(decoded.length == (static_cast<ULONGLONG>(length) << shifts[encoding]));
            EXPECT_TRUE(decoded.physicalAddress.QuadPart == 0x2800000000LL);
            EXPECT_TRUE(decoded.flags == descriptor.Flags);
        }
        EXPECT_TRUE(DecodeMemoryResource(Memory(0x1000, 0, encodings[encoding]),
            &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);
        EXPECT_TRUE(decoded.length == 0);
        EXPECT_TRUE(DecodeMemoryResource(Memory(0x1000, NV_U32_MAX, encodings[encoding]),
            &decoded) == STATUS_SUCCESS);
        EXPECT_TRUE(decoded.length == (static_cast<ULONGLONG>(NV_U32_MAX) << shifts[encoding]));
        if (encoding != 0)
        {
            EXPECT_TRUE(DecodeMemoryResource(Memory(0x2800000000,
                static_cast<ULONG>(0x400000000ULL >> shifts[encoding]),
                encodings[encoding]), &decoded) == STATUS_SUCCESS);
            EXPECT_TRUE(decoded.length == 0x400000000ULL);
        }
    }
    auto invalid = Memory(0x1000, 1);
    invalid.Type = CmResourceTypeMemoryLarge;
    EXPECT_TRUE(DecodeMemoryResource(invalid, &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);
    invalid.Flags = CM_RESOURCE_MEMORY_LARGE_40 | CM_RESOURCE_MEMORY_LARGE_48;
    EXPECT_TRUE(DecodeMemoryResource(invalid, &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);
    invalid.Type = CmResourceTypePort;
    EXPECT_TRUE(DecodeMemoryResource(invalid, &decoded) == STATUS_INVALID_PARAMETER);
    EXPECT_TRUE(DecodeMemoryResource(invalid, nullptr) == STATUS_INVALID_PARAMETER);
    EXPECT_TRUE(DecodeMemoryResource(Memory(NV_U64_MAX - 15, 16), &decoded) == STATUS_SUCCESS);
    EXPECT_TRUE(DecodeMemoryResource(Memory(NV_U64_MAX - 15, 17),
        &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);

    std::array<CM_PARTIAL_RESOURCE_DESCRIPTOR, 4> raw = {
        Memory(0x97000000, 0x1000000),
        {},
        Memory(0x2800000000, 4, CM_RESOURCE_MEMORY_LARGE_64),
        Memory(0x2FFE000000, 0x2000000) };
    raw[1].Type = CmResourceTypeInterrupt;
    auto translated = raw;
    translated[0].u.Memory.Start.QuadPart += 0x10000000;
    translated[2].u.Memory64.Start.QuadPart += 0x1000000000;
    translated[3].u.Memory.Start.QuadPart += 0x1000000000;
    auto resolve = [&](ULONGLONG address, ULONG rawCount = 4, ULONG translatedCount = 4) {
        return nv::platform::ResolveMemoryBar(address, rawCount, translatedCount,
            [&](ULONG index) { return &raw[index]; },
            [&](ULONG index) { return &translated[index]; }, &decoded);
    };
    EXPECT_TRUE(resolve(0x97000000) == STATUS_SUCCESS);
    EXPECT_TRUE(decoded.physicalAddress.QuadPart == 0xA7000000);
    EXPECT_TRUE(resolve(0x2800000000) == STATUS_SUCCESS);
    EXPECT_TRUE(decoded.physicalAddress.QuadPart == 0x3800000000);
    EXPECT_TRUE(decoded.length == 0x400000000ULL);
    for (unsigned encoding = 1; encoding < 4; ++encoding)
    {
        raw[2] = Memory(0x2800000000,
            static_cast<ULONG>(0x400000000ULL >> shifts[encoding]), encodings[encoding]);
        translated[2] = Memory(0x3800000000,
            static_cast<ULONG>(0x400000000ULL >> shifts[encoding]), encodings[encoding]);
        EXPECT_TRUE(resolve(0x2800000000) == STATUS_SUCCESS);
        EXPECT_TRUE(decoded.physicalAddress.QuadPart == 0x3800000000);
        EXPECT_TRUE(decoded.length == 0x400000000ULL);
    }
    EXPECT_TRUE(resolve(0x2FFE000000) == STATUS_SUCCESS);
    EXPECT_TRUE(decoded.physicalAddress.QuadPart == 0x3FFE000000);
    EXPECT_TRUE(resolve(0x12345000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    EXPECT_TRUE(resolve(0) == STATUS_DEVICE_CONFIGURATION_ERROR);
    EXPECT_TRUE(resolve(0x97000000, 3, 4) == STATUS_DEVICE_CONFIGURATION_ERROR);
    const auto saved = translated[2];
    translated[2].Type = CmResourceTypeInterrupt;
    EXPECT_TRUE(resolve(0x2800000000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    translated[2] = saved;
    ++translated[2].u.Memory64.Length64;
    EXPECT_TRUE(resolve(0x2800000000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    translated[2] = saved;
    raw[1] = raw[2];
    translated[1] = translated[2];
    EXPECT_TRUE(resolve(0x2800000000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    raw[1] = {};
    EXPECT_TRUE(nv::platform::ResolveMemoryBar(0x2800000000, 4, 4,
        [&](ULONG index) -> const CM_PARTIAL_RESOURCE_DESCRIPTOR* {
            return index == 1 ? nullptr : &raw[index];
        }, [&](ULONG index) { return &translated[index]; }, &decoded) ==
        STATUS_DEVICE_CONFIGURATION_ERROR);

    for (ULONG pageSize : { 4096UL, 65536UL })
    {
        const SIZE_T maximum =
            static_cast<SIZE_T>(NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH) & ~(pageSize - 1ULL);
        EXPECT_TRUE(nv::platform::IsSupportedUserMmioRange(0x2800000000, maximum, pageSize));
        EXPECT_TRUE(!nv::platform::IsSupportedUserMmioRange(0x2800000000, maximum + 1, pageSize));
        for (ULONG offset = 0; offset < pageSize; ++offset)
        {
            EXPECT_TRUE(nv::platform::IsSupportedUserMmioRange(0x2800000000 + offset,
                maximum - offset, pageSize));
            EXPECT_TRUE(!nv::platform::IsSupportedUserMmioRange(0x2800000000 + offset,
                maximum - offset + 1, pageSize));
        }
    }
    EXPECT_TRUE(!nv::platform::IsSupportedUserMmioRange(0x1000, 0, 4096));
    EXPECT_TRUE(!nv::platform::IsSupportedUserMmioRange(0x1000, 1, 0));
    EXPECT_TRUE(!nv::platform::IsSupportedUserMmioRange(0x1000, 1, 3000));
}

struct Mapping;

struct Backend
{
    unsigned maps = 0;
    unsigned unmaps = 0;
    NTSTATUS failure = STATUS_SUCCESS;
    bool incomplete = false;
    unsigned active = 0;
    unsigned duplicateMaps = 0;
    ULONGLONG lastStart = 0;
    SIZE_T lastLength = 0;
    std::unique_ptr<Mapping> mapping;
};

struct Mapping
{
    std::array<UCHAR, 64> bytes = {};
    Backend* backend = nullptr;
    void* reservedAddress = nullptr;
};

UCHAR Read8(void* context, SIZE_T offset)
{
    return static_cast<Mapping*>(context)->bytes[offset % 64];
}

USHORT Read16(void* context, SIZE_T offset)
{
    return static_cast<USHORT>(Read8(context, offset) | (Read8(context, offset + 1) << 8));
}

ULONG Read32(void* context, SIZE_T offset)
{
    return Read16(context, offset) | (static_cast<ULONG>(Read16(context, offset + 2)) << 16);
}

void Read32Buffer(void* context, SIZE_T offset, ULONG* data, ULONG count)
{
    for (ULONG index = 0; index < count; ++index)
    {
        data[index] = Read32(context, offset + index * sizeof(ULONG));
    }
}

void Write8(void* context, SIZE_T offset, UCHAR value)
{
    static_cast<Mapping*>(context)->bytes[offset % 64] = value;
}

void Write16(void* context, SIZE_T offset, USHORT value)
{
    Write8(context, offset, static_cast<UCHAR>(value));
    Write8(context, offset + 1, static_cast<UCHAR>(value >> 8));
}

void Write32(void* context, SIZE_T offset, ULONG value)
{
    Write16(context, offset, static_cast<USHORT>(value));
    Write16(context, offset + 2, static_cast<USHORT>(value >> 16));
}

NV_PLATFORM_MMIO_ACCESS Access(Mapping* mapping)
{
    return { mapping, Read8, Read16, Read32, Read32Buffer, Write8, Write16, Write32 };
}

NTSTATUS Map(void* context, ULONGLONG start, SIZE_T length,
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType, volatile void** address,
    NV_PLATFORM_MMIO_ACCESS* access)
{
    auto& backend = *static_cast<Backend*>(context);
    ++backend.maps;
    if (backend.failure != STATUS_SUCCESS)
    {
        return backend.failure;
    }
    if (backend.active != 0)
    {
        ++backend.duplicateMaps;
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (cacheType != NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED ||
        !nv::platform::IsSupportedUserMmioRange(start, length, 4096))
    {
        return STATUS_INVALID_PARAMETER;
    }
    auto mapping = std::make_unique<Mapping>();
    // Reserve pointer identity only; callbacks access the small RAM array,
    // never the inaccessible reservation or a real hardware aperture.
    mapping->backend = &backend;
    mapping->reservedAddress = VirtualAlloc(nullptr, length, MEM_RESERVE, PAGE_NOACCESS);
    if (mapping->reservedAddress == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ++backend.active;
    backend.lastStart = start;
    backend.lastLength = length;
    *access = Access(mapping.get());
    *address = backend.incomplete ? nullptr : mapping->reservedAddress;
    backend.mapping = std::move(mapping);
    return STATUS_SUCCESS;
}

void Unmap(void* context, void* mappingContext)
{
    auto& backend = *static_cast<Backend*>(context);
    auto* mapping = static_cast<Mapping*>(mappingContext);
    ASSERT_EQ(mapping, backend.mapping.get());
    ASSERT_NE(mapping, nullptr);
    EXPECT_TRUE(mapping->backend == &backend);
    ++backend.unmaps;
    --backend.active;
    EXPECT_TRUE(VirtualFree(mapping->reservedAddress, 0, MEM_RELEASE) != FALSE);
    backend.mapping.reset();
}

NV_STATUS MapPhysical(NvU64 start, NvU64 length, void** address,
    NvU32 cache = NV_MEMORY_WRITECOMBINED)
{
    return NvPlatformRmMapPhysical(start, length, NV_PROTECT_READ_WRITE, cache, address);
}

TEST(BarResources, OwnsDemandMappingsAndConcurrentLeases)
{
    // Even disjoint leases share one OS mapping. A request outside its window
    // must fail while aliases are live, rather than create a second mapping.
    constexpr NvU64 Base = 0x2800000000;
    constexpr NvU64 Size = 0x400000000;
    Backend backend;
    const NV_PLATFORM_MMIO_MAPPING_PROVIDER provider = { &backend, Map, Unmap };
    void* resource = nullptr;
    EXPECT_TRUE(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_SUCCESS);
    EXPECT_TRUE(backend.maps == 0);
    void* address = nullptr;
    EXPECT_TRUE(MapPhysical(Base + 0x200000000, 4096, &address) == NV_OK);
    EXPECT_TRUE(backend.maps == 1);
    EXPECT_TRUE(backend.lastLength == NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH);
    EXPECT_TRUE(backend.lastStart <= Base + 0x200000000);
    EXPECT_TRUE(Base + 0x200000000 - backend.lastStart + 4096 <= backend.lastLength);
    void* alias = nullptr;
    EXPECT_TRUE(MapPhysical(Base + 0x200000000 + 8, 8, &alias) == NV_OK);
    EXPECT_TRUE(alias == static_cast<UCHAR*>(address) + 8);
    EXPECT_TRUE(backend.maps == 1);
    void* distantAlias = nullptr;
    EXPECT_TRUE(MapPhysical(Base + 0x200100000, 4096, &distantAlias) == NV_OK);
    EXPECT_TRUE(distantAlias == static_cast<UCHAR*>(address) + 0x100000);
    EXPECT_TRUE(backend.maps == 1);
    void* outside = nullptr;
    EXPECT_TRUE(MapPhysical(Base, 4096, &outside) == NV_ERR_STATE_IN_USE);
    EXPECT_TRUE(outside == nullptr);
    EXPECT_TRUE(backend.maps == 1 && backend.duplicateMaps == 0);
    std::array<UCHAR, 8> input = { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::array<UCHAR, 8> output = {};
    EXPECT_TRUE(NvPlatformRmCopyMemory(alias, input.data(), input.size()) == NV_TRUE);
    EXPECT_TRUE(NvPlatformRmCopyMemory(output.data(), alias, output.size()) == NV_TRUE);
    EXPECT_TRUE(input == output);
    NvPlatformRmUnmapPhysical(address);
    EXPECT_TRUE(backend.unmaps == 0);
    NvPlatformRmUnmapPhysical(alias);
    EXPECT_TRUE(backend.unmaps == 0);
    NvPlatformRmUnmapPhysical(distantAlias);
    EXPECT_TRUE(backend.unmaps == 1);
    EXPECT_TRUE(MapPhysical(Base + 0x100000000 - 2048, 4096, &address) == NV_OK);
    EXPECT_TRUE(backend.lastStart == Base + 0x100000000 - 4096);
    NvPlatformRmUnmapPhysical(address);
    EXPECT_TRUE(MapPhysical(Base + Size - 4096, 4096, &address) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    EXPECT_TRUE(MapPhysical(Base, NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH, &address) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    EXPECT_TRUE(MapPhysical(Base, NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH + 1, &address) ==
        NV_ERR_INVALID_LIMIT);
    EXPECT_TRUE(address == nullptr);
    EXPECT_TRUE(MapPhysical(Base, 0x100000000, &address) == NV_ERR_INVALID_LIMIT);
    EXPECT_TRUE(MapPhysical(Base - 1, 1, &address) == NV_ERR_OBJECT_NOT_FOUND);
    EXPECT_TRUE(MapPhysical(Base + Size, 1, &address) == NV_ERR_OBJECT_NOT_FOUND);
    EXPECT_TRUE(MapPhysical(Base + Size - 1, 2, &address) == NV_ERR_OBJECT_NOT_FOUND);
    EXPECT_TRUE(MapPhysical(NV_U64_MAX - 15, 17, &address) == NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(MapPhysical(Base, 0, &address) == NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(MapPhysical(Base, 1, nullptr) == NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(MapPhysical(Base, 1, &address, NV_MEMORY_CACHED) == NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(MapPhysical(Base, 1, &address, NV_MEMORY_UNCACHED) == NV_ERR_OBJECT_NOT_FOUND);
    EXPECT_TRUE(NvPlatformRmMapPhysical(Base, 1, 0, NV_MEMORY_DEFAULT, &address) ==
        NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(MapPhysical(Base + 1, NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH, &address) ==
        NV_ERR_INVALID_LIMIT);
    backend.failure = STATUS_INSUFFICIENT_RESOURCES;
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_ERR_NO_MEMORY);
    backend.failure = STATUS_ACCESS_DENIED;
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_ERR_OPERATING_SYSTEM);
    backend.failure = STATUS_SUCCESS;
    backend.incomplete = true;
    const unsigned unmaps = backend.unmaps;
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_ERR_OPERATING_SYSTEM);
    EXPECT_TRUE(backend.unmaps == unmaps + 1);
    backend.incomplete = false;

    std::atomic<unsigned> failures{0};
    std::array<std::thread, 8> threads;
    for (auto& thread : threads)
    {
        thread = std::thread([&] {
            for (unsigned index = 0; index < 1000; ++index)
            {
                void* mapped = nullptr;
                if (MapPhysical(Base + 0x300000000, 4096, &mapped) != NV_OK)
                {
                    ++failures;
                }
                else
                {
                    NvPlatformRmUnmapPhysical(mapped);
                }
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    EXPECT_TRUE(failures == 0);
    EXPECT_TRUE(MapPhysical(Base, 4096, &address, NV_MEMORY_DEFAULT) == NV_OK);
    const unsigned beforeRemoval = backend.unmaps;
    NvPlatformUnregisterMmioResource(resource);
    EXPECT_TRUE(backend.unmaps == beforeRemoval + 1);
    NvPlatformRmUnmapPhysical(address);
    EXPECT_TRUE(backend.unmaps == beforeRemoval + 1);
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_ERR_OBJECT_NOT_FOUND);

    Backend otherBackend;
    const NV_PLATFORM_MMIO_MAPPING_PROVIDER otherProvider = { &otherBackend, Map, Unmap };
    void* otherResource = nullptr;
    constexpr NvU64 OtherBase = Base + 0x1000000000;
    EXPECT_TRUE(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_SUCCESS);
    EXPECT_TRUE(NvPlatformRegisterMmioResource(OtherBase, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &otherProvider, &otherResource) == STATUS_SUCCESS);
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_OK);
    EXPECT_TRUE(MapPhysical(OtherBase, 4096, &alias) == NV_OK);
    NvPlatformUnregisterMmioResource(resource);
    EXPECT_TRUE(otherBackend.unmaps == 0);
    void* otherAlias = nullptr;
    EXPECT_TRUE(MapPhysical(OtherBase + 1, 1, &otherAlias) == NV_OK);
    EXPECT_TRUE(otherBackend.maps == 1);
    NvPlatformRmUnmapPhysical(alias);
    EXPECT_TRUE(otherBackend.unmaps == 0);
    NvPlatformRmUnmapPhysical(otherAlias);
    EXPECT_TRUE(otherBackend.unmaps == 1);
    NvPlatformUnregisterMmioResource(otherResource);

    Mapping eager;
    const auto access = Access(&eager);
    std::array<void*, 32> registrations = {};
    for (unsigned index = 0; index < registrations.size(); ++index)
    {
        EXPECT_TRUE(NvPlatformRegisterMmioAperture(0x97000000ULL + index * 4096,
            eager.bytes.data(), eager.bytes.size(), NV_PLATFORM_MMIO_CACHE_UNCACHED,
            &access, &registrations[index]) == STATUS_SUCCESS);
    }
    EXPECT_TRUE(MapPhysical(0x97000000, 8, &address, NV_MEMORY_UNCACHED) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    EXPECT_TRUE(MapPhysical(0x97000000, 8, &address, NV_MEMORY_UNCACHED_WEAK) == NV_OK);
    EXPECT_TRUE(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_SUCCESS);
    const unsigned beforeExhaustion = backend.maps;
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_ERR_NO_MEMORY);
    EXPECT_TRUE(backend.maps == beforeExhaustion);
    for (void* registration : registrations) { NvPlatformUnregisterMmioAperture(registration); }
    EXPECT_TRUE(MapPhysical(Base, 4096, &address) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    NvPlatformUnregisterMmioResource(resource);
    for (auto& registration : registrations)
    {
        EXPECT_TRUE(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
            &provider, &registration) == STATUS_SUCCESS);
    }
    EXPECT_TRUE(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_INSUFFICIENT_RESOURCES);
    for (void* registration : registrations) { NvPlatformUnregisterMmioResource(registration); }
    EXPECT_TRUE(NvPlatformRegisterMmioResource(NV_U64_MAX - 15, 17,
        NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED, &provider, &resource) == STATUS_INVALID_PARAMETER);
    EXPECT_TRUE(NvPlatformRegisterMmioResource(Base, 0, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_INVALID_PARAMETER);
    NvPlatformUnregisterMmioResource(nullptr);
    NvPlatformRmUnmapPhysical(nullptr);
    // Map counts attempts: the two injected failures acquired nothing to unmap.
    EXPECT_TRUE(backend.maps == backend.unmaps + 2);
    EXPECT_TRUE(backend.active == 0 && backend.duplicateMaps == 0);
    EXPECT_TRUE(otherBackend.maps == otherBackend.unmaps);
}

} // namespace
