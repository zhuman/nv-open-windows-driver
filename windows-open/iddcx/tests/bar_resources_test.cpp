#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
extern "C" {
#include <wudfwdm.h>
}

#include "../../platform/kernel/nvplatform_memory_resource.hpp"
#include "../../platform/kernel/nvplatform_mmio_user.h"
#include "../../platform/kernel/nvplatform_mmio_rm.h"
#include "kernel/os/nv_memory_type.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace {

unsigned cases = 0;

void Check(bool condition)
{
    ++cases;
    if (!condition)
    {
        std::fprintf(stderr, "BAR resource/MMIO case %u failed\n", cases);
        std::abort();
    }
}

CM_PARTIAL_RESOURCE_DESCRIPTOR Memory(ULONGLONG address, ULONG length, USHORT large = 0)
{
    CM_PARTIAL_RESOURCE_DESCRIPTOR descriptor = {};
    descriptor.Type = large == 0 ? CmResourceTypeMemory : CmResourceTypeMemoryLarge;
    descriptor.Flags = large | CM_RESOURCE_MEMORY_PREFETCHABLE;
    descriptor.u.Memory.Start.QuadPart = static_cast<LONGLONG>(address);
    descriptor.u.Memory.Length = length;
    return descriptor;
}

void TestResources()
{
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
            Check(DecodeMemoryResource(descriptor, &decoded) == STATUS_SUCCESS);
            Check(decoded.length == (static_cast<ULONGLONG>(length) << shifts[encoding]));
            Check(decoded.physicalAddress.QuadPart == 0x2800000000LL);
            Check(decoded.flags == descriptor.Flags);
        }
        Check(DecodeMemoryResource(Memory(0x1000, 0, encodings[encoding]),
            &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);
        Check(decoded.length == 0);
        Check(DecodeMemoryResource(Memory(0x1000, NV_U32_MAX, encodings[encoding]),
            &decoded) == STATUS_SUCCESS);
        Check(decoded.length == (static_cast<ULONGLONG>(NV_U32_MAX) << shifts[encoding]));
        if (encoding != 0)
        {
            Check(DecodeMemoryResource(Memory(0x2800000000,
                static_cast<ULONG>(0x400000000ULL >> shifts[encoding]),
                encodings[encoding]), &decoded) == STATUS_SUCCESS);
            Check(decoded.length == 0x400000000ULL);
        }
    }
    auto invalid = Memory(0x1000, 1);
    invalid.Type = CmResourceTypeMemoryLarge;
    Check(DecodeMemoryResource(invalid, &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);
    invalid.Flags = CM_RESOURCE_MEMORY_LARGE_40 | CM_RESOURCE_MEMORY_LARGE_48;
    Check(DecodeMemoryResource(invalid, &decoded) == STATUS_DEVICE_CONFIGURATION_ERROR);
    invalid.Type = CmResourceTypePort;
    Check(DecodeMemoryResource(invalid, &decoded) == STATUS_INVALID_PARAMETER);
    Check(DecodeMemoryResource(invalid, nullptr) == STATUS_INVALID_PARAMETER);
    Check(DecodeMemoryResource(Memory(NV_U64_MAX - 15, 16), &decoded) == STATUS_SUCCESS);
    Check(DecodeMemoryResource(Memory(NV_U64_MAX - 15, 17),
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
    Check(resolve(0x97000000) == STATUS_SUCCESS);
    Check(decoded.physicalAddress.QuadPart == 0xA7000000);
    Check(resolve(0x2800000000) == STATUS_SUCCESS);
    Check(decoded.physicalAddress.QuadPart == 0x3800000000);
    Check(decoded.length == 0x400000000ULL);
    for (unsigned encoding = 1; encoding < 4; ++encoding)
    {
        raw[2] = Memory(0x2800000000,
            static_cast<ULONG>(0x400000000ULL >> shifts[encoding]), encodings[encoding]);
        translated[2] = Memory(0x3800000000,
            static_cast<ULONG>(0x400000000ULL >> shifts[encoding]), encodings[encoding]);
        Check(resolve(0x2800000000) == STATUS_SUCCESS);
        Check(decoded.physicalAddress.QuadPart == 0x3800000000);
        Check(decoded.length == 0x400000000ULL);
    }
    Check(resolve(0x2FFE000000) == STATUS_SUCCESS);
    Check(decoded.physicalAddress.QuadPart == 0x3FFE000000);
    Check(resolve(0x12345000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    Check(resolve(0) == STATUS_DEVICE_CONFIGURATION_ERROR);
    Check(resolve(0x97000000, 3, 4) == STATUS_DEVICE_CONFIGURATION_ERROR);
    const auto saved = translated[2];
    translated[2].Type = CmResourceTypeInterrupt;
    Check(resolve(0x2800000000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    translated[2] = saved;
    ++translated[2].u.Memory64.Length64;
    Check(resolve(0x2800000000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    translated[2] = saved;
    raw[1] = raw[2];
    translated[1] = translated[2];
    Check(resolve(0x2800000000) == STATUS_DEVICE_CONFIGURATION_ERROR);
    raw[1] = {};
    Check(nv::platform::ResolveMemoryBar(0x2800000000, 4, 4,
        [&](ULONG index) -> const CM_PARTIAL_RESOURCE_DESCRIPTOR* {
            return index == 1 ? nullptr : &raw[index];
        }, [&](ULONG index) { return &translated[index]; }, &decoded) ==
        STATUS_DEVICE_CONFIGURATION_ERROR);

    for (ULONG pageSize : { 4096UL, 65536UL })
    {
        const SIZE_T maximum =
            static_cast<SIZE_T>(NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH) & ~(pageSize - 1ULL);
        Check(nv::platform::IsSupportedUserMmioRange(0x2800000000, maximum, pageSize));
        Check(!nv::platform::IsSupportedUserMmioRange(0x2800000000, maximum + 1, pageSize));
        for (ULONG offset = 0; offset < pageSize; ++offset)
        {
            Check(nv::platform::IsSupportedUserMmioRange(0x2800000000 + offset,
                maximum - offset, pageSize));
            Check(!nv::platform::IsSupportedUserMmioRange(0x2800000000 + offset,
                maximum - offset + 1, pageSize));
        }
    }
    Check(!nv::platform::IsSupportedUserMmioRange(0x1000, 0, 4096));
    Check(!nv::platform::IsSupportedUserMmioRange(0x1000, 1, 0));
    Check(!nv::platform::IsSupportedUserMmioRange(0x1000, 1, 3000));
}

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
    auto* mapping = new Mapping{};
    mapping->backend = &backend;
    mapping->reservedAddress = VirtualAlloc(nullptr, length, MEM_RESERVE, PAGE_NOACCESS);
    if (mapping->reservedAddress == nullptr)
    {
        delete mapping;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ++backend.active;
    backend.lastStart = start;
    backend.lastLength = length;
    *access = Access(mapping);
    *address = backend.incomplete ? nullptr : mapping->reservedAddress;
    return STATUS_SUCCESS;
}

void Unmap(void* context)
{
    auto* mapping = static_cast<Mapping*>(context);
    ++mapping->backend->unmaps;
    --mapping->backend->active;
    Check(VirtualFree(mapping->reservedAddress, 0, MEM_RELEASE) != FALSE);
    delete mapping;
}

NV_STATUS MapPhysical(NvU64 start, NvU64 length, void** address,
    NvU32 cache = NV_MEMORY_WRITECOMBINED)
{
    return NvPlatformRmMapPhysical(start, length, NV_PROTECT_READ_WRITE, cache, address);
}

void TestMappings()
{
    constexpr NvU64 Base = 0x2800000000;
    constexpr NvU64 Size = 0x400000000;
    Backend backend;
    const NV_PLATFORM_MMIO_MAPPING_PROVIDER provider = { &backend, Map, Unmap };
    void* resource = nullptr;
    Check(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_SUCCESS);
    Check(backend.maps == 0);
    void* address = nullptr;
    Check(MapPhysical(Base + 0x200000000, 4096, &address) == NV_OK);
    Check(backend.maps == 1);
    Check(backend.lastLength == NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH);
    Check(backend.lastStart <= Base + 0x200000000);
    Check(Base + 0x200000000 - backend.lastStart + 4096 <= backend.lastLength);
    void* alias = nullptr;
    Check(MapPhysical(Base + 0x200000000 + 8, 8, &alias) == NV_OK);
    Check(alias == static_cast<UCHAR*>(address) + 8);
    Check(backend.maps == 1);
    void* distantAlias = nullptr;
    Check(MapPhysical(Base + 0x200100000, 4096, &distantAlias) == NV_OK);
    Check(distantAlias == static_cast<UCHAR*>(address) + 0x100000);
    Check(backend.maps == 1);
    void* outside = nullptr;
    Check(MapPhysical(Base, 4096, &outside) == NV_ERR_STATE_IN_USE);
    Check(outside == nullptr);
    Check(backend.maps == 1 && backend.duplicateMaps == 0);
    std::array<UCHAR, 8> input = { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::array<UCHAR, 8> output = {};
    Check(NvPlatformRmCopyMemory(alias, input.data(), input.size()) == NV_TRUE);
    Check(NvPlatformRmCopyMemory(output.data(), alias, output.size()) == NV_TRUE);
    Check(input == output);
    NvPlatformRmUnmapPhysical(address);
    Check(backend.unmaps == 0);
    NvPlatformRmUnmapPhysical(alias);
    Check(backend.unmaps == 0);
    NvPlatformRmUnmapPhysical(distantAlias);
    Check(backend.unmaps == 1);
    Check(MapPhysical(Base + 0x100000000 - 2048, 4096, &address) == NV_OK);
    Check(backend.lastStart == Base + 0x100000000 - 4096);
    NvPlatformRmUnmapPhysical(address);
    Check(MapPhysical(Base + Size - 4096, 4096, &address) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    Check(MapPhysical(Base, NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH, &address) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    Check(MapPhysical(Base, NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH + 1, &address) ==
        NV_ERR_INVALID_LIMIT);
    Check(address == nullptr);
    Check(MapPhysical(Base, 0x100000000, &address) == NV_ERR_INVALID_LIMIT);
    Check(MapPhysical(Base - 1, 1, &address) == NV_ERR_OBJECT_NOT_FOUND);
    Check(MapPhysical(Base + Size, 1, &address) == NV_ERR_OBJECT_NOT_FOUND);
    Check(MapPhysical(Base + Size - 1, 2, &address) == NV_ERR_OBJECT_NOT_FOUND);
    Check(MapPhysical(NV_U64_MAX - 15, 17, &address) == NV_ERR_INVALID_ARGUMENT);
    Check(MapPhysical(Base, 0, &address) == NV_ERR_INVALID_ARGUMENT);
    Check(MapPhysical(Base, 1, nullptr) == NV_ERR_INVALID_ARGUMENT);
    Check(MapPhysical(Base, 1, &address, NV_MEMORY_CACHED) == NV_ERR_INVALID_ARGUMENT);
    Check(MapPhysical(Base, 1, &address, NV_MEMORY_UNCACHED) == NV_ERR_OBJECT_NOT_FOUND);
    Check(NvPlatformRmMapPhysical(Base, 1, 0, NV_MEMORY_DEFAULT, &address) ==
        NV_ERR_INVALID_ARGUMENT);
    Check(MapPhysical(Base + 1, NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH, &address) ==
        NV_ERR_INVALID_LIMIT);
    backend.failure = STATUS_INSUFFICIENT_RESOURCES;
    Check(MapPhysical(Base, 4096, &address) == NV_ERR_NO_MEMORY);
    backend.failure = STATUS_ACCESS_DENIED;
    Check(MapPhysical(Base, 4096, &address) == NV_ERR_OPERATING_SYSTEM);
    backend.failure = STATUS_SUCCESS;
    backend.incomplete = true;
    const unsigned unmaps = backend.unmaps;
    Check(MapPhysical(Base, 4096, &address) == NV_ERR_OPERATING_SYSTEM);
    Check(backend.unmaps == unmaps + 1);
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
    Check(failures == 0);
    Check(MapPhysical(Base, 4096, &address, NV_MEMORY_DEFAULT) == NV_OK);
    const unsigned beforeRemoval = backend.unmaps;
    NvPlatformUnregisterMmioResource(resource);
    Check(backend.unmaps == beforeRemoval + 1);
    NvPlatformRmUnmapPhysical(address);
    Check(backend.unmaps == beforeRemoval + 1);
    Check(MapPhysical(Base, 4096, &address) == NV_ERR_OBJECT_NOT_FOUND);

    Backend otherBackend;
    const NV_PLATFORM_MMIO_MAPPING_PROVIDER otherProvider = { &otherBackend, Map, Unmap };
    void* otherResource = nullptr;
    constexpr NvU64 OtherBase = Base + 0x1000000000;
    Check(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_SUCCESS);
    Check(NvPlatformRegisterMmioResource(OtherBase, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &otherProvider, &otherResource) == STATUS_SUCCESS);
    Check(MapPhysical(Base, 4096, &address) == NV_OK);
    Check(MapPhysical(OtherBase, 4096, &alias) == NV_OK);
    NvPlatformUnregisterMmioResource(resource);
    Check(otherBackend.unmaps == 0);
    void* otherAlias = nullptr;
    Check(MapPhysical(OtherBase + 1, 1, &otherAlias) == NV_OK);
    Check(otherBackend.maps == 1);
    NvPlatformRmUnmapPhysical(alias);
    Check(otherBackend.unmaps == 0);
    NvPlatformRmUnmapPhysical(otherAlias);
    Check(otherBackend.unmaps == 1);
    NvPlatformUnregisterMmioResource(otherResource);

    Mapping eager;
    const auto access = Access(&eager);
    std::array<void*, 32> registrations = {};
    for (unsigned index = 0; index < registrations.size(); ++index)
    {
        Check(NvPlatformRegisterMmioAperture(0x97000000ULL + index * 4096,
            eager.bytes.data(), eager.bytes.size(), NV_PLATFORM_MMIO_CACHE_UNCACHED,
            &access, &registrations[index]) == STATUS_SUCCESS);
    }
    Check(MapPhysical(0x97000000, 8, &address, NV_MEMORY_UNCACHED) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    Check(MapPhysical(0x97000000, 8, &address, NV_MEMORY_UNCACHED_WEAK) == NV_OK);
    Check(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_SUCCESS);
    const unsigned beforeExhaustion = backend.maps;
    Check(MapPhysical(Base, 4096, &address) == NV_ERR_NO_MEMORY);
    Check(backend.maps == beforeExhaustion);
    for (void* registration : registrations) { NvPlatformUnregisterMmioAperture(registration); }
    Check(MapPhysical(Base, 4096, &address) == NV_OK);
    NvPlatformRmUnmapPhysical(address);
    NvPlatformUnregisterMmioResource(resource);
    for (auto& registration : registrations)
    {
        Check(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
            &provider, &registration) == STATUS_SUCCESS);
    }
    Check(NvPlatformRegisterMmioResource(Base, Size, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_INSUFFICIENT_RESOURCES);
    for (void* registration : registrations) { NvPlatformUnregisterMmioResource(registration); }
    Check(NvPlatformRegisterMmioResource(NV_U64_MAX - 15, 17,
        NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED, &provider, &resource) == STATUS_INVALID_PARAMETER);
    Check(NvPlatformRegisterMmioResource(Base, 0, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED,
        &provider, &resource) == STATUS_INVALID_PARAMETER);
    NvPlatformUnregisterMmioResource(nullptr);
    NvPlatformRmUnmapPhysical(nullptr);
    Check(backend.maps == backend.unmaps + 2);
    Check(backend.active == 0 && backend.duplicateMaps == 0);
    Check(otherBackend.maps == otherBackend.unmaps);
}

} // namespace

int main()
{
    TestResources();
    TestMappings();
    std::printf("PASS: %u resource/mapping checks and 8000 concurrent map/unmap cycles\n", cases);
}
