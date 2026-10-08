#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include "../pci_bars.hpp"
#include "../../platform/kernel/nvplatform_mmio_rm.h"
#include "kernel/os/nv_memory_type.h"
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace {

using namespace nv::iddcx;

unsigned cases = 0;
unsigned traceErrors = 0;

void Check(bool condition)
{
    ++cases;
    if (!condition)
    {
        std::fprintf(stderr, "PCI BAR ownership case %u failed\n", cases);
        std::abort();
    }
}

struct ResourceList
{
    std::array<CM_PARTIAL_RESOURCE_DESCRIPTOR, 3> descriptors = {};
    ULONG count = 3;

    WDFCMRESLIST Handle() noexcept
    {
        return reinterpret_cast<WDFCMRESLIST>(this);
    }
};

struct Mapping
{
    void* address = nullptr;
    ~Mapping()
    {
        if (address != nullptr)
        {
            Check(VirtualFree(address, 0, MEM_RELEASE) != FALSE);
        }
    }
};

struct Fixture;
Fixture* current = nullptr;

struct Fixture
{
    ResourceList raw;
    ResourceList translated;
    ConfigSpace config = {};
    std::array<std::unique_ptr<Mapping>, 3> mappings;
    std::array<unsigned, 3> maps = {};
    std::array<unsigned, 3> unmaps = {};
    ULONG configCount = 3;
    ULONG boot0 = 0x190000A1;
    ULONG boot42 = 0x00000001;
    int failingBar = -1;

    Fixture()
    {
        Check(current == nullptr);
        current = this;
        const ULONGLONG bases[] = { 0x97000000, 0x2800000000, 0x2FFE000000 };
        const ULONG lengths[] = { 0x1000000, 4, 0x2000000 };
        for (ULONG index = 0; index < 3; ++index)
        {
            auto& descriptor = raw.descriptors[index];
            descriptor.Type = index == 1 ? CmResourceTypeMemoryLarge : CmResourceTypeMemory;
            descriptor.Flags = index == 1 ? CM_RESOURCE_MEMORY_LARGE_64 : 0;
            descriptor.u.Memory.Start.QuadPart = static_cast<LONGLONG>(bases[index]);
            descriptor.u.Memory.Length = lengths[index];
        }
        translated = raw;
        for (auto& descriptor : translated.descriptors)
        {
            descriptor.u.Memory.Start.QuadPart += 0x1000000000;
        }
        Check(config.Initialize(Device()) == STATUS_SUCCESS);
    }
    ~Fixture()
    {
        for (ULONG index = 0; index < 3; ++index)
        {
            Check(!mappings[index]);
            Check(maps[index] == unmaps[index]);
        }
        current = nullptr;
    }
    WDFDEVICE Device() noexcept { return reinterpret_cast<WDFDEVICE>(this); }
    NTSTATUS Initialize(PciBars& bars)
    {
        return bars.Initialize(Device(), raw.Handle(), translated.Handle(), config);
    }
    nv::platform::MemoryResource Resource(ULONG index) const
    {
        nv::platform::MemoryResource resource = {};
        Check(nv::platform::DecodeMemoryResource(translated.descriptors[index], &resource) ==
            STATUS_SUCCESS);
        return resource;
    }
};

ULONG NTAPI ResourceCount(PWDF_DRIVER_GLOBALS, WDFCMRESLIST list)
{
    return reinterpret_cast<ResourceList*>(list)->count;
}

PCM_PARTIAL_RESOURCE_DESCRIPTOR NTAPI ResourceDescriptor(
    PWDF_DRIVER_GLOBALS, WDFCMRESLIST list, ULONG index)
{
    auto& resources = *reinterpret_cast<ResourceList*>(list);
    return index < resources.count ? &resources.descriptors[index] : nullptr;
}

std::array<WDFFUNC, WdfFunctionTableNumEntries> wdfFunctions = {};

UCHAR Read8(void*, SIZE_T) { return 0; }
USHORT Read16(void*, SIZE_T) { return 0; }
ULONG Read32(void*, SIZE_T) { return 0; }
void ReadBuffer32(void*, SIZE_T, ULONG*, ULONG) {}
void Write8(void*, SIZE_T, UCHAR) {}
void Write16(void*, SIZE_T, USHORT) {}
void Write32(void*, SIZE_T, ULONG) {}

NV_STATUS MapBar1(const Fixture& fixture, void** address)
{
    return NvPlatformRmMapPhysical(
        static_cast<NvU64>(fixture.Resource(1).physicalAddress.QuadPart) + 4096,
        4096, NV_PROTECT_READ_WRITE, NV_MEMORY_WRITECOMBINED, address);
}

void TestOwnership()
{
    Fixture fixture;
    void* lease = nullptr;
    {
        PciBars bars;
        Check(fixture.Initialize(bars) == STATUS_SUCCESS);
        Check(bars.Count() == 3);
        for (ULONG index = 0; index < bars.Count(); ++index)
        {
            const auto expected = fixture.Resource(index);
            Check(bars[index].Resource().physicalAddress.QuadPart == expected.physicalAddress.QuadPart);
            Check(bars[index].Resource().length == expected.length);
            Check((bars[index].Address() != nullptr) == (index != 1));
        }
        Check(bars[1].Resource().length == 0x400000000ULL);
        Check(MapBar1(fixture, &lease) == NV_OK);
        Check(fixture.maps[1] == 1);
        Check(bars[1].Aperture().Length() == NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH);
        void* alias = nullptr;
        Check(MapBar1(fixture, &alias) == NV_OK);
        Check(alias == lease);
        Check(fixture.maps[1] == 1);
        NvPlatformRmUnmapPhysical(lease);
        Check(fixture.unmaps[1] == 0);
    }
    Check(fixture.unmaps[1] == 1);
    Check(MapBar1(fixture, &lease) == NV_ERR_OBJECT_NOT_FOUND);
    Check(lease == nullptr);
}

void TestRecovery()
{
    Fixture fixture;
    PciBars bars;
    Check(fixture.Initialize(bars) == STATUS_SUCCESS);
    const auto bar0Address = bars[PciBars::Bar0Index].Address();
    const auto bar2Address = bars[PciBars::Bar2Index].Address();
    void* lease = nullptr;
    Check(MapBar1(fixture, &lease) == NV_OK);
    bars.SuspendDemandMappings();
    Check(bars.Count() == 3);
    Check(fixture.unmaps[1] == 1);
    Check(bars[1].Address() == nullptr);
    Check(MapBar1(fixture, &lease) == NV_ERR_OBJECT_NOT_FOUND);
    Check(bars.RestoreDemandMappings() == STATUS_SUCCESS);
    Check(bars[PciBars::Bar0Index].Address() == bar0Address);
    Check(bars[PciBars::Bar2Index].Address() == bar2Address);
    Check(fixture.maps[0] == 1 && fixture.unmaps[0] == 0);
    Check(fixture.maps[2] == 1 && fixture.unmaps[2] == 0);
    Check(MapBar1(fixture, &lease) == NV_OK);
    NvPlatformRmUnmapPhysical(lease);
    Check(fixture.unmaps[1] == 2);
    bars.Reset();
    bars.Reset();
    Check(bars.Count() == 0);
    Check(fixture.Initialize(bars) == STATUS_SUCCESS);
}

void TestFailures()
{
    for (int failingBar : { 0, 2 })
    {
        Fixture fixture;
        fixture.failingBar = failingBar;
        PciBars bars;
        const unsigned errors = traceErrors;
        Check(fixture.Initialize(bars) == STATUS_INSUFFICIENT_RESOURCES);
        Check(bars.Count() == 0);
        Check(traceErrors > errors);
        fixture.failingBar = -1;
        Check(fixture.Initialize(bars) == STATUS_SUCCESS);
    }
    for (unsigned failure = 0; failure < 5; ++failure)
    {
        Fixture fixture;
        PciBars bars;
        const unsigned errors = traceErrors;
        if (failure == 0)
        {
            fixture.boot0 = fixture.boot42 = 0xFFFFFFFF;
        }
        else if (failure == 1)
        {
            fixture.translated.count = 2;
        }
        else if (failure == 2)
        {
            fixture.raw.descriptors[0].u.Memory.Length = 4;
            fixture.translated.descriptors[0].u.Memory.Length = 4;
        }
        else if (failure == 3)
        {
            ++fixture.translated.descriptors[1].u.Memory.Start.QuadPart;
        }
        else
        {
            fixture.configCount = 2;
            Check(fixture.config.Initialize(fixture.Device()) == STATUS_SUCCESS);
        }
        Check(fixture.Initialize(bars) < 0);
        Check(bars.Count() == 0);
        Check(traceErrors > errors);
        void* lease = nullptr;
        Check(MapBar1(fixture, &lease) == NV_ERR_OBJECT_NOT_FOUND);
    }
    {
        Fixture fixture;
        PciBars bars;
        const unsigned errors = traceErrors;
        Check(bars.RestoreDemandMappings() == STATUS_INVALID_DEVICE_STATE);
        Check(traceErrors > errors);
        Check(fixture.Initialize(bars) == STATUS_SUCCESS);
        void* lease = nullptr;
        Check(MapBar1(fixture, &lease) == NV_OK);
        Check(bars.RestoreDemandMappings() == STATUS_INVALID_DEVICE_STATE);
        Check(fixture.unmaps[1] == 0);
        void* alias = nullptr;
        Check(MapBar1(fixture, &alias) == NV_OK);
        Check(alias == lease);
        Check(fixture.maps[1] == 1);
        NvPlatformRmUnmapPhysical(lease);
        NvPlatformRmUnmapPhysical(alias);
        bars.SuspendDemandMappings();
        Check(bars.RestoreDemandMappings() == STATUS_SUCCESS);
    }
}

} // namespace

const WDFFUNC* WdfFunctions = wdfFunctions.data();
PWDF_DRIVER_GLOBALS WdfDriverGlobals = nullptr;

void RecordBarTrace(UCHAR level) noexcept
{
    if (level == 2)
    {
        ++traceErrors;
    }
}

namespace nv::iddcx {

NTSTATUS ConfigSpace::Initialize(WDFDEVICE device) noexcept
{
    auto& fixture = *reinterpret_cast<Fixture*>(device);
    memoryBarCount_ = fixture.configCount;
    for (ULONG index = 0; index < memoryBarCount_; ++index)
    {
        memoryBarAddresses_[index] = fixture.raw.descriptors[index].u.Memory.Start;
    }
    return STATUS_SUCCESS;
}

ULONG ConfigSpace::MemoryBarCount() const noexcept { return memoryBarCount_; }
PHYSICAL_ADDRESS ConfigSpace::MemoryBarAddress(ULONG index) const noexcept
{
    return memoryBarAddresses_[index];
}

NTSTATUS RegisterAperture::InitializeRange(
    WDFDEVICE device, PHYSICAL_ADDRESS physicalAddress, SIZE_T length,
    MEMORY_CACHING_TYPE cacheType) noexcept
{
    auto& fixture = *reinterpret_cast<Fixture*>(device);
    Check(current == &fixture);
    Reset();
    Check(nv::platform::IsSupportedUserMmioRange(
        static_cast<ULONGLONG>(physicalAddress.QuadPart), length, 4096));
    for (ULONG index = 0; index < 3; ++index)
    {
        const auto resource = fixture.Resource(index);
        const auto offset = physicalAddress.QuadPart - resource.physicalAddress.QuadPart;
        if (offset < 0 || static_cast<ULONGLONG>(offset) >= resource.length)
        {
            continue;
        }
        Check(!fixture.mappings[index]);
        Check(cacheType == (index == 0 ? MmNonCached : MmWriteCombined));
        if (fixture.failingBar == static_cast<int>(index))
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        auto mapping = std::make_unique<Mapping>();
        mapping->address = VirtualAlloc(nullptr, length, MEM_RESERVE, PAGE_NOACCESS);
        Check(mapping->address != nullptr);
        device_ = device;
        address_ = mapping->address;
        pseudoAddress_ = mapping.get();
        length_ = length;
        physicalAddress_ = physicalAddress;
        fixture.mappings[index] = std::move(mapping);
        ++fixture.maps[index];
        return STATUS_SUCCESS;
    }
    Check(false);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

void RegisterAperture::Reset() noexcept
{
    if (device_ != nullptr)
    {
        auto& fixture = *reinterpret_cast<Fixture*>(device_);
        for (ULONG index = 0; index < 3; ++index)
        {
            if (fixture.mappings[index].get() == pseudoAddress_)
            {
                fixture.mappings[index].reset();
                ++fixture.unmaps[index];
                break;
            }
        }
    }
    device_ = nullptr;
    address_ = nullptr;
    pseudoAddress_ = nullptr;
    length_ = 0;
    physicalAddress_ = {};
}

NTSTATUS RegisterAperture::Read32(SIZE_T offset, ULONG* value) const noexcept
{
    if (address_ == nullptr || offset > length_ || sizeof(*value) > length_ - offset)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *value = offset == 0 ? current->boot0 : current->boot42;
    return STATUS_SUCCESS;
}

volatile void* RegisterAperture::Address() const noexcept { return address_; }
SIZE_T RegisterAperture::Length() const noexcept { return length_; }
PHYSICAL_ADDRESS RegisterAperture::PhysicalAddress() const noexcept { return physicalAddress_; }
NV_PLATFORM_MMIO_ACCESS RegisterAperture::Access() noexcept
{
    return { this, Read8, Read16, ::Read32, ReadBuffer32, Write8, Write16, Write32 };
}

} // namespace nv::iddcx

int main()
{
    static_assert(!std::is_copy_constructible_v<PciBars>);
    static_assert(!std::is_move_constructible_v<PciBars>);
    static_assert(!std::is_copy_constructible_v<PciBar>);
    static_assert(!std::is_move_constructible_v<PciBar>);
    static_assert(!std::is_copy_constructible_v<RegisterAperture>);
    static_assert(!std::is_move_constructible_v<RegisterAperture>);
    wdfFunctions[WdfCmResourceListGetCountTableIndex] = reinterpret_cast<WDFFUNC>(ResourceCount);
    wdfFunctions[WdfCmResourceListGetDescriptorTableIndex] = reinterpret_cast<WDFFUNC>(ResourceDescriptor);
    TestOwnership();
    TestRecovery();
    TestFailures();
    std::printf("PASS: %u production PCI BAR ownership/lifecycle checks (mock WDF/config backend)\n", cases);
}
