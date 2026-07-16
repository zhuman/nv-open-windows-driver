#pragma once

#include <windows.h>
#include <wdf.h>

extern "C" {
#include <wudfwdm.h>
}

namespace nv::iddcx {

struct PciIdentity final
{
    USHORT vendorId;
    USHORT deviceId;
    USHORT subsystemVendorId;
    USHORT subsystemId;
    UCHAR revisionId;
    UCHAR programmingInterface;
    UCHAR subclass;
    UCHAR baseClass;
};

class ConfigSpace final
{
public:
    NTSTATUS Initialize(_In_ WDFDEVICE device) noexcept;
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

private:
    BUS_INTERFACE_STANDARD interface_;
    PciIdentity identity_;
    bool identityValid_;
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

private:
    static EVT_WDF_INTERRUPT_ISR Isr;
    static EVT_WDF_INTERRUPT_DPC Dpc;

    WDFINTERRUPT handle_;
};

} // namespace nv::iddcx