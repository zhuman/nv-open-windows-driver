#include "pci_bars.hpp"
#include "trace.h"

#include <new>

#include "pci_bars.tmh"

namespace {

constexpr SIZE_T PmcBoot0Offset = 0x00000000;
constexpr SIZE_T PmcBoot42Offset = 0x00000A00;
constexpr SIZE_T RequiredIdentityLength = PmcBoot42Offset + sizeof(ULONG);
constexpr ULONG InvalidRegisterValue = static_cast<ULONG>(-1);

} // namespace

namespace nv::iddcx {

NTSTATUS PciBar::Initialize(
    WDFDEVICE device,
    WDFCMRESLIST resourcesRaw,
    WDFCMRESLIST resourcesTranslated,
    ULONG index,
    PHYSICAL_ADDRESS configAddress,
    MappingMode mode,
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
    SIZE_T minimumLength) noexcept
{
    Reset();
    if (device == nullptr || resourcesRaw == nullptr || resourcesTranslated == nullptr)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "BAR%u initialization requires device and resource lists", index);
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = nv::platform::ResolveMemoryBar(
        static_cast<ULONGLONG>(configAddress.QuadPart),
        WdfCmResourceListGetCount(resourcesRaw),
        WdfCmResourceListGetCount(resourcesTranslated),
        [resourcesRaw](ULONG resourceIndex) {
            return WdfCmResourceListGetDescriptor(resourcesRaw, resourceIndex);
        },
        [resourcesTranslated](ULONG resourceIndex) {
            return WdfCmResourceListGetDescriptor(resourcesTranslated, resourceIndex);
        },
        &resource_);
    TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "BAR%u resource: status=%!STATUS! config=0x%llX translated=0x%llX length=0x%llX flags=0x%X",
        index, status, configAddress.QuadPart, resource_.physicalAddress.QuadPart,
        resource_.length, resource_.flags);
    if (!NT_SUCCESS(status))
    {
        Reset();
        return status;
    }
    if (resource_.length < minimumLength ||
        (mode == MappingMode::Eager &&
         resource_.length > NV_PLATFORM_MAX_USER_MMIO_MAPPING_LENGTH))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "BAR%u resource length incompatible with mapping policy: length=0x%llX",
            index, resource_.length);
        Reset();
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device_ = device;
    index_ = index;
    mode_ = mode;
    cacheType_ = cacheType;
    status = Register();
    if (!NT_SUCCESS(status))
    {
        Reset();
    }
    return status;
}

NTSTATUS PciBar::Register() noexcept
{
    if (device_ == nullptr || resource_.length == 0 ||
        apertureRegistration_ || resourceRegistration_ || aperture_)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "BAR%u registration requires an initialized, unregistered resource", index_);
        return STATUS_INVALID_DEVICE_STATE;
    }
    void* registration = nullptr;
    NTSTATUS status;
    if (mode_ == MappingMode::OnDemand)
    {
        const NV_PLATFORM_MMIO_MAPPING_PROVIDER provider = {
            this, MapRange, UnmapRange };
        status = NvPlatformRegisterMmioResource(
            static_cast<ULONGLONG>(resource_.physicalAddress.QuadPart),
            resource_.length, cacheType_, &provider, &registration);
        resourceRegistration_.reset(registration);
    }
    else
    {
        volatile void* address = nullptr;
        NV_PLATFORM_MMIO_ACCESS access = {};
        status = MapRange(this,
            static_cast<ULONGLONG>(resource_.physicalAddress.QuadPart),
            static_cast<SIZE_T>(resource_.length), cacheType_, &address, &access);
        if (NT_SUCCESS(status))
        {
            status = NvPlatformRegisterMmioAperture(
                static_cast<ULONGLONG>(resource_.physicalAddress.QuadPart),
                address, static_cast<SIZE_T>(resource_.length), cacheType_,
                &access, &registration);
            apertureRegistration_.reset(registration);
        }
    }
    TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI,
        "BAR%u registration: status=%!STATUS! start=0x%llX length=0x%llX demand=%u",
        index_, status, resource_.physicalAddress.QuadPart, resource_.length,
        mode_ == MappingMode::OnDemand ? 1U : 0U);
    return status;
}

NTSTATUS PciBar::MapRange(
    void* context,
    ULONGLONG physicalAddress,
    SIZE_T length,
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
    volatile void** address,
    NV_PLATFORM_MMIO_ACCESS* access)
{
    *address = nullptr;
    *access = {};
    auto& bar = *static_cast<PciBar*>(context);
    const ULONGLONG start = static_cast<ULONGLONG>(bar.resource_.physicalAddress.QuadPart);
    if (bar.aperture_ || cacheType != bar.cacheType_ || physicalAddress < start ||
        length == 0 || physicalAddress - start > bar.resource_.length ||
        length > bar.resource_.length - (physicalAddress - start))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "BAR%u mapping conflicts with resource, cache policy, or active mapping",
            bar.index_);
        return STATUS_INVALID_DEVICE_STATE;
    }
    std::unique_ptr<RegisterAperture> aperture(new (std::nothrow) RegisterAperture{});
    if (!aperture)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI, "BAR%u mapping allocation failed", bar.index_);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PHYSICAL_ADDRESS startAddress = {};
    startAddress.QuadPart = static_cast<LONGLONG>(physicalAddress);
    const NTSTATUS status = aperture->InitializeRange(
        bar.device_, startAddress, length,
        cacheType == NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED ? MmWriteCombined : MmNonCached);
    TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_PCI, "BAR%u mapping: status=%!STATUS! start=0x%llX length=0x%llX",
        bar.index_, status, physicalAddress, static_cast<ULONGLONG>(length));
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    *address = aperture->Address();
    *access = aperture->Access();
    bar.aperture_ = std::move(aperture);
    return STATUS_SUCCESS;
}

void PciBar::UnmapRange(void* context, void* mappingContext)
{
    auto& bar = *static_cast<PciBar*>(context);
    if (!bar.aperture_ || bar.aperture_.get() != mappingContext)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "BAR%u unmap does not match the owned mapping", bar.index_);
        return;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_PCI,
        "BAR%u unmap: start=0x%llX length=0x%llX", bar.index_,
        bar.aperture_->PhysicalAddress().QuadPart,
        static_cast<ULONGLONG>(bar.aperture_->Length()));
    bar.aperture_.reset();
}

void PciBar::Unregister() noexcept
{
    resourceRegistration_.reset();
    apertureRegistration_.reset();
    aperture_.reset();
}

void PciBar::Reset() noexcept
{
    Unregister();
    device_ = nullptr;
    index_ = 0;
    resource_ = {};
    mode_ = MappingMode::Eager;
    cacheType_ = NV_PLATFORM_MMIO_CACHE_UNCACHED;
}

NTSTATUS PciBars::Initialize(
    WDFDEVICE device,
    WDFCMRESLIST resourcesRaw,
    WDFCMRESLIST resourcesTranslated,
    const ConfigSpace& configSpace) noexcept
{
    Reset();
    if (configSpace.MemoryBarCount() != MaximumCount)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "GPU requires %u memory BARs; found %u", MaximumCount, configSpace.MemoryBarCount());
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    struct Policy
    {
        PciBar::MappingMode mode;
        NV_PLATFORM_MMIO_CACHE_TYPE cacheType;
        SIZE_T minimumLength;
    };
    static constexpr std::array<Policy, MaximumCount> policies = {{
        { PciBar::MappingMode::Eager, NV_PLATFORM_MMIO_CACHE_UNCACHED, RequiredIdentityLength },
        { PciBar::MappingMode::OnDemand, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED, 1 },
        { PciBar::MappingMode::Eager, NV_PLATFORM_MMIO_CACHE_WRITE_COMBINED, 1 },
    }};
    auto cleanup = wil::scope_exit([this]() noexcept { Reset(); });
    for (ULONG index = 0; index < MaximumCount; ++index)
    {
        const auto& policy = policies[index];
        NTSTATUS status = bars_[index].Initialize(
            device, resourcesRaw, resourcesTranslated, index,
            configSpace.MemoryBarAddress(index), policy.mode, policy.cacheType,
            policy.minimumLength);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (index == Bar0Index)
        {
            ULONG boot0 = 0;
            ULONG boot42 = 0;
            status = bars_[Bar0Index].Aperture().Read32(PmcBoot0Offset, &boot0);
            if (NT_SUCCESS(status))
            {
                status = bars_[Bar0Index].Aperture().Read32(PmcBoot42Offset, &boot42);
            }
            if (NT_SUCCESS(status) && boot0 == InvalidRegisterValue && boot42 == InvalidRegisterValue)
            {
                status = STATUS_DEVICE_DOES_NOT_EXIST;
            }
            TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
                TRACE_PCI, "BAR0 identity: status=%!STATUS! boot0=0x%08X boot42=0x%08X",
                status, boot0, boot42);
            if (!NT_SUCCESS(status))
            {
                return status;
            }
        }
    }
    count_ = MaximumCount;
    cleanup.release();
    return STATUS_SUCCESS;
}

void PciBars::Reset() noexcept
{
    // Drain demand leases before unmapping fixed apertures.
    SuspendDemandMappings();
    for (ULONG index = MaximumCount; index != 0; --index)
    {
        bars_[index - 1].Reset();
    }
    count_ = 0;
}

void PciBars::SuspendDemandMappings() noexcept
{
    for (auto& bar : bars_)
    {
        if (bar.mode_ == PciBar::MappingMode::OnDemand)
        {
            bar.Unregister();
        }
    }
}

NTSTATUS PciBars::RestoreDemandMappings() noexcept
{
    if (count_ != MaximumCount)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
            "Cannot restore demand mappings before BAR initialization");
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (const auto& bar : bars_)
    {
        if (bar.mode_ == PciBar::MappingMode::OnDemand &&
            (bar.resourceRegistration_ || bar.aperture_))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_PCI,
                "BAR%u demand mappings must be suspended before restoration", bar.index_);
            return STATUS_INVALID_DEVICE_STATE;
        }
    }
    for (ULONG index = 0; index < count_; ++index)
    {
        auto& bar = bars_[index];
        if (bar.mode_ == PciBar::MappingMode::OnDemand)
        {
            const NTSTATUS status = bar.Register();
            if (!NT_SUCCESS(status))
            {
                SuspendDemandMappings();
                return status;
            }
        }
    }
    return STATUS_SUCCESS;
}

} // namespace nv::iddcx
