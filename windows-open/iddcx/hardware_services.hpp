#pragma once

#include <windows.h>
#include <wdf.h>

extern "C" {
#include <wudfwdm.h>
}

namespace nv::iddcx {

struct PciIdentity final
{
    USHORT domain;
    USHORT vendorId;
    USHORT deviceId;
    USHORT subsystemVendorId;
    USHORT subsystemId;
    UCHAR bus;
    UCHAR device;
    UCHAR function;
    UCHAR revisionId;
    UCHAR programmingInterface;
    UCHAR subclass;
    UCHAR baseClass;
};

struct BootIdentity final
{
    ULONG boot0;
    ULONG boot42;
};

class ConfigSpace final
{
public:
    static constexpr ULONG MaximumMemoryBarCount = 3;

    NTSTATUS Initialize(_In_ WDFDEVICE device) noexcept;
    NTSTATUS EnableMemorySpace() noexcept;
    void Reset() noexcept;

    NTSTATUS Read(
        _In_ ULONG offset,
        _Out_writes_bytes_(length) void* buffer,
        _In_ ULONG length) const noexcept;

    NTSTATUS Write(
        _In_ ULONG offset,
        _In_reads_bytes_(length) const void* buffer,
        _In_ ULONG length) const noexcept;

    [[nodiscard]] bool IsAvailable() const noexcept;
    [[nodiscard]] const PciIdentity* Identity() const noexcept;
    [[nodiscard]] ULONG MemoryBarCount() const noexcept;
    [[nodiscard]] PHYSICAL_ADDRESS MemoryBarAddress(_In_ ULONG index) const noexcept;
    [[nodiscard]] PHYSICAL_ADDRESS RegisterBarAddress() const noexcept;

private:
    BUS_INTERFACE_STANDARD interface_;
    PciIdentity identity_;
    PHYSICAL_ADDRESS memoryBarAddresses_[MaximumMemoryBarCount];
    ULONG memoryBarCount_;
    bool identityValid_;
    bool memorySpaceEnabledByDriver_;
};

class RegisterAperture final
{
public:
    NTSTATUS Initialize(
        _In_ WDFDEVICE device,
        _In_ WDFCMRESLIST resourcesTranslated,
        _In_ const ConfigSpace& configSpace) noexcept;

    void Reset() noexcept;

    NTSTATUS Read32(_In_ SIZE_T offset, _Out_ ULONG* value) const noexcept;

    [[nodiscard]] bool IsMapped() const noexcept;
    [[nodiscard]] volatile void* Address() const noexcept;
    [[nodiscard]] SIZE_T Length() const noexcept;
    [[nodiscard]] const BootIdentity* Identity() const noexcept;

private:
    WDFDEVICE device_;
    void* address_;
    SIZE_T length_;
    BootIdentity identity_;
};

class DmaEngine final
{
public:
    NTSTATUS Initialize(_In_ WDFDEVICE device) noexcept;
    void Reset() noexcept;

    [[nodiscard]] WDFDMAENABLER Handle() const noexcept;

private:
    WDFDMAENABLER handle_;
};

class Interrupt final
{
public:
    NTSTATUS Initialize(
        _In_ WDFDEVICE device,
        _In_ WDFCMRESLIST resourcesRaw,
        _In_ WDFCMRESLIST resourcesTranslated) noexcept;

    void Reset() noexcept;

    [[nodiscard]] WDFINTERRUPT Handle() const noexcept;
    [[nodiscard]] ULONG Vector() const noexcept;
    [[nodiscard]] ULONG Count() const noexcept;

private:
    static EVT_WDF_INTERRUPT_ISR Isr;
    static EVT_WDF_INTERRUPT_DPC Dpc;

    WDFINTERRUPT handle_;
    ULONG vector_;
    ULONG count_;
};

} // namespace nv::iddcx