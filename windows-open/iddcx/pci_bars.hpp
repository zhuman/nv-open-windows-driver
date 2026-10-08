#pragma once

#include "hardware_services.hpp"
#include "nvplatform_memory_resource.hpp"

#include <array>
#include <memory>
#include <wil/resource.h>

namespace nv::iddcx {

class PciBars;

class PciBar final
{
public:
    PciBar() noexcept = default;
    ~PciBar() noexcept { Reset(); }
    PciBar(const PciBar&) = delete;
    PciBar& operator=(const PciBar&) = delete;
    PciBar(PciBar&&) = delete;
    PciBar& operator=(PciBar&&) = delete;

    [[nodiscard]] const nv::platform::MemoryResource& Resource() const noexcept
    {
        return resource_;
    }
    [[nodiscard]] volatile void* Address() const noexcept
    {
        return aperture_ ? aperture_->Address() : nullptr;
    }
    [[nodiscard]] const RegisterAperture& Aperture() const noexcept
    {
        return *aperture_;
    }

private:
    friend class PciBars;

    enum class MappingMode { Eager, OnDemand };

    NTSTATUS Initialize(
        WDFDEVICE device,
        WDFCMRESLIST resourcesRaw,
        WDFCMRESLIST resourcesTranslated,
        ULONG index,
        PHYSICAL_ADDRESS configAddress,
        MappingMode mode,
        NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
        SIZE_T minimumLength) noexcept;
    NTSTATUS Register() noexcept;
    void Unregister() noexcept;
    void Reset() noexcept;

    static NTSTATUS MapRange(
        void* context,
        ULONGLONG physicalAddress,
        SIZE_T length,
        NV_PLATFORM_MMIO_CACHE_TYPE cacheType,
        volatile void** address,
        NV_PLATFORM_MMIO_ACCESS* access);
    static void UnmapRange(void* context, void* mappingContext);

    using unique_mmio_aperture = wil::unique_any<
        void*, decltype(&NvPlatformUnregisterMmioAperture),
        NvPlatformUnregisterMmioAperture>;
    using unique_mmio_resource = wil::unique_any<
        void*, decltype(&NvPlatformUnregisterMmioResource),
        NvPlatformUnregisterMmioResource>;

    WDFDEVICE device_ = nullptr;
    ULONG index_ = 0;
    nv::platform::MemoryResource resource_ = {};
    MappingMode mode_ = MappingMode::Eager;
    NV_PLATFORM_MMIO_CACHE_TYPE cacheType_ = NV_PLATFORM_MMIO_CACHE_UNCACHED;
    std::unique_ptr<RegisterAperture> aperture_;
    unique_mmio_aperture apertureRegistration_;
    unique_mmio_resource resourceRegistration_;
};

class PciBars final
{
public:
    PciBars() noexcept = default;
    ~PciBars() noexcept { Reset(); }
    PciBars(const PciBars&) = delete;
    PciBars& operator=(const PciBars&) = delete;
    PciBars(PciBars&&) = delete;
    PciBars& operator=(PciBars&&) = delete;

    static constexpr ULONG Bar0Index = 0;
    static constexpr ULONG Bar1Index = 1;
    static constexpr ULONG Bar2Index = 2;
    static constexpr ULONG MaximumCount = ConfigSpace::MaximumMemoryBarCount;

    NTSTATUS Initialize(
        WDFDEVICE device,
        WDFCMRESLIST resourcesRaw,
        WDFCMRESLIST resourcesTranslated,
        const ConfigSpace& configSpace) noexcept;
    void Reset() noexcept;
    void SuspendDemandMappings() noexcept;
    NTSTATUS RestoreDemandMappings() noexcept;

    [[nodiscard]] ULONG Count() const noexcept { return count_; }
    [[nodiscard]] const PciBar& operator[](ULONG index) const noexcept
    {
        return bars_[index];
    }

private:
    std::array<PciBar, MaximumCount> bars_;
    ULONG count_ = 0;
};

} // namespace nv::iddcx
