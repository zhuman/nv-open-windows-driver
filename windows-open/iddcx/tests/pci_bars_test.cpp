#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <gtest/gtest.h>

#include "../pci_bars.hpp"
#include "../../platform/kernel/nvplatform_mmio_rm.h"
#include "kernel/os/nv_memory_type.h"
#include <type_traits>

namespace {

using namespace nv::iddcx;

unsigned traceErrors = 0;


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
            EXPECT_TRUE(VirtualFree(address, 0, MEM_RELEASE) != FALSE);
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
        EXPECT_TRUE(current == nullptr);
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
        EXPECT_TRUE(config.Initialize(Device()) == STATUS_SUCCESS);
    }
    ~Fixture()
    {
        for (ULONG index = 0; index < 3; ++index)
        {
            EXPECT_TRUE(!mappings[index]);
            EXPECT_TRUE(maps[index] == unmaps[index]);
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
        EXPECT_TRUE(nv::platform::DecodeMemoryResource(translated.descriptors[index], &resource) ==
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

class PciBarsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        traceErrors = 0;
        wdfFunctions[WdfCmResourceListGetCountTableIndex] =
            reinterpret_cast<WDFFUNC>(ResourceCount);
        wdfFunctions[WdfCmResourceListGetDescriptorTableIndex] =
            reinterpret_cast<WDFFUNC>(ResourceDescriptor);
    }
};

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

TEST_F(PciBarsTest, OwnsBarsAndDemandMappingLeases)
{
    // BAR destruction revokes the provider even if a client kept an alias;
    // the underlying mapping must still be released exactly once.
    Fixture fixture;
    void* lease = nullptr;
    {
        PciBars bars;
        EXPECT_TRUE(fixture.Initialize(bars) == STATUS_SUCCESS);
        EXPECT_TRUE(bars.Count() == 3);
        for (ULONG index = 0; index < bars.Count(); ++index)
        {
            const auto expected = fixture.Resource(index);
            EXPECT_TRUE(bars[index].Resource().physicalAddress.QuadPart == expected.physicalAddress.QuadPart);
            EXPECT_TRUE(bars[index].Resource().length == expected.length);
            EXPECT_TRUE((bars[index].Address() != nullptr) == (index != 1));
        }
        EXPECT_TRUE(bars[1].Resource().length == 0x400000000ULL);
        EXPECT_TRUE(MapBar1(fixture, &lease) == NV_OK);
        EXPECT_TRUE(fixture.maps[1] == 1);
        EXPECT_TRUE(bars[1].Aperture().Length() == NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH);
        void* alias = nullptr;
        EXPECT_TRUE(MapBar1(fixture, &alias) == NV_OK);
        EXPECT_TRUE(alias == lease);
        EXPECT_TRUE(fixture.maps[1] == 1);
        NvPlatformRmUnmapPhysical(lease);
        EXPECT_TRUE(fixture.unmaps[1] == 0);
    }
    EXPECT_TRUE(fixture.unmaps[1] == 1);
    EXPECT_TRUE(MapBar1(fixture, &lease) == NV_ERR_OBJECT_NOT_FOUND);
    EXPECT_TRUE(lease == nullptr);
}

TEST_F(PciBarsTest, SuspendsRestoresAndReinitializesMappings)
{
    // Demand-provider suspension must preserve fixed BAR0/BAR2 addresses.
    Fixture fixture;
    PciBars bars;
    EXPECT_TRUE(fixture.Initialize(bars) == STATUS_SUCCESS);
    const auto bar0Address = bars[PciBars::Bar0Index].Address();
    const auto bar2Address = bars[PciBars::Bar2Index].Address();
    void* lease = nullptr;
    EXPECT_TRUE(MapBar1(fixture, &lease) == NV_OK);
    bars.SuspendDemandMappings();
    EXPECT_TRUE(bars.Count() == 3);
    EXPECT_TRUE(fixture.unmaps[1] == 1);
    EXPECT_TRUE(bars[1].Address() == nullptr);
    EXPECT_TRUE(MapBar1(fixture, &lease) == NV_ERR_OBJECT_NOT_FOUND);
    EXPECT_TRUE(bars.RestoreDemandMappings() == STATUS_SUCCESS);
    EXPECT_TRUE(bars[PciBars::Bar0Index].Address() == bar0Address);
    EXPECT_TRUE(bars[PciBars::Bar2Index].Address() == bar2Address);
    EXPECT_TRUE(fixture.maps[0] == 1 && fixture.unmaps[0] == 0);
    EXPECT_TRUE(fixture.maps[2] == 1 && fixture.unmaps[2] == 0);
    EXPECT_TRUE(MapBar1(fixture, &lease) == NV_OK);
    NvPlatformRmUnmapPhysical(lease);
    EXPECT_TRUE(fixture.unmaps[1] == 2);
    bars.Reset();
    bars.Reset();
    EXPECT_TRUE(bars.Count() == 0);
    EXPECT_TRUE(fixture.Initialize(bars) == STATUS_SUCCESS);
}

TEST_F(PciBarsTest, RollsBackInitializationAndRejectsInvalidRestores)
{
    for (int failingBar : { 0, 2 })
    {
        Fixture fixture;
        fixture.failingBar = failingBar;
        PciBars bars;
        const unsigned errors = traceErrors;
        EXPECT_TRUE(fixture.Initialize(bars) == STATUS_INSUFFICIENT_RESOURCES);
        EXPECT_TRUE(bars.Count() == 0);
        EXPECT_TRUE(traceErrors > errors);
        fixture.failingBar = -1;
        EXPECT_TRUE(fixture.Initialize(bars) == STATUS_SUCCESS);
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
            EXPECT_TRUE(fixture.config.Initialize(fixture.Device()) == STATUS_SUCCESS);
        }
        EXPECT_TRUE(fixture.Initialize(bars) < 0);
        EXPECT_TRUE(bars.Count() == 0);
        EXPECT_TRUE(traceErrors > errors);
        void* lease = nullptr;
        EXPECT_TRUE(MapBar1(fixture, &lease) == NV_ERR_OBJECT_NOT_FOUND);
    }
    {
        Fixture fixture;
        PciBars bars;
        const unsigned errors = traceErrors;
        EXPECT_TRUE(bars.RestoreDemandMappings() == STATUS_INVALID_DEVICE_STATE);
        EXPECT_TRUE(traceErrors > errors);
        EXPECT_TRUE(fixture.Initialize(bars) == STATUS_SUCCESS);
        void* lease = nullptr;
        EXPECT_TRUE(MapBar1(fixture, &lease) == NV_OK);
        // Reject duplicate restoration without draining the existing aliases.
        EXPECT_TRUE(bars.RestoreDemandMappings() == STATUS_INVALID_DEVICE_STATE);
        EXPECT_TRUE(fixture.unmaps[1] == 0);
        void* alias = nullptr;
        EXPECT_TRUE(MapBar1(fixture, &alias) == NV_OK);
        EXPECT_TRUE(alias == lease);
        EXPECT_TRUE(fixture.maps[1] == 1);
        NvPlatformRmUnmapPhysical(lease);
        NvPlatformRmUnmapPhysical(alias);
        bars.SuspendDemandMappings();
        EXPECT_TRUE(bars.RestoreDemandMappings() == STATUS_SUCCESS);
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
    EXPECT_TRUE(current == &fixture);
    Reset();
    EXPECT_TRUE(nv::platform::IsSupportedUserMmioRange(
        static_cast<ULONGLONG>(physicalAddress.QuadPart), length, 4096));
    for (ULONG index = 0; index < 3; ++index)
    {
        const auto resource = fixture.Resource(index);
        const auto offset = physicalAddress.QuadPart - resource.physicalAddress.QuadPart;
        if (offset < 0 || static_cast<ULONGLONG>(offset) >= resource.length)
        {
            continue;
        }
        EXPECT_TRUE(!fixture.mappings[index]);
        EXPECT_TRUE(cacheType == (index == 0 ? MmNonCached : MmWriteCombined));
        if (fixture.failingBar == static_cast<int>(index))
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        auto mapping = std::make_unique<Mapping>();
        mapping->address = VirtualAlloc(nullptr, length, MEM_RESERVE, PAGE_NOACCESS);
        if (mapping->address == nullptr)
        {
            ADD_FAILURE() << "Unable to reserve fake BAR aperture";
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        device_ = device;
        address_ = mapping->address;
        pseudoAddress_ = mapping.get();
        length_ = length;
        physicalAddress_ = physicalAddress;
        fixture.mappings[index] = std::move(mapping);
        ++fixture.maps[index];
        return STATUS_SUCCESS;
    }
    EXPECT_TRUE(false);
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

static_assert(!std::is_copy_constructible_v<PciBars>);
static_assert(!std::is_move_constructible_v<PciBars>);
static_assert(!std::is_copy_constructible_v<PciBar>);
static_assert(!std::is_move_constructible_v<PciBar>);
static_assert(!std::is_copy_constructible_v<RegisterAperture>);
static_assert(!std::is_move_constructible_v<RegisterAperture>);
