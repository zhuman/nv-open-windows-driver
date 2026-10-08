#pragma once

#include <windows.h>
#include <wdf.h>

extern "C" {
#include <wudfwdm.h>
}

#include "nvplatform_mmio_user.h"

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

using DeviceResetHandler = NTSTATUS (NTAPI*)(
    _In_ void* context,
    _In_ ULONG resetType,
    _In_ ULONG flags,
    _In_opt_ void* resetParameters);

union UmBusSpecificResetType
{
    ULONGLONG value;
};

struct UmBusSpecificResetInfo
{
    GUID busType;
    UmBusSpecificResetType supported;
};

union UmDeviceResetStatusFlags
{
    ULONGLONG value;
};

union UmBusSpecificResetFlags
{
    ULONGLONG value;
};

static_assert(sizeof(UmBusSpecificResetType) == sizeof(ULONGLONG));
static_assert(sizeof(UmDeviceResetStatusFlags) == sizeof(ULONGLONG));
static_assert(sizeof(UmBusSpecificResetFlags) == sizeof(ULONGLONG));
static_assert(sizeof(UmBusSpecificResetInfo) == 24);

using QueryBusSpecificResetHandler = NTSTATUS (NTAPI*)(
    void* context, ULONG* count, UmBusSpecificResetInfo* info);
using GetDeviceResetStatusHandler = NTSTATUS (NTAPI*)(
    void* context, BOOLEAN* resetting, UmBusSpecificResetType* selected,
    UmDeviceResetStatusFlags* flags);
using BusSpecificResetHandler = NTSTATUS (NTAPI*)(
    void* context, const GUID* busType, UmBusSpecificResetType selected,
    UmBusSpecificResetFlags* flags, void* parameters);

struct UmDeviceResetInterface final
{
    USHORT size;
    USHORT version;
    void* context;
    PINTERFACE_REFERENCE interfaceReference;
    PINTERFACE_DEREFERENCE interfaceDereference;
    DeviceResetHandler deviceReset;
    ULONG supportedResetTypes;
    void* reserved;
    QueryBusSpecificResetHandler queryBusSpecificResetInfo;
    BusSpecificResetHandler deviceBusSpecificReset;
    GetDeviceResetStatusHandler getDeviceResetStatus;
};

class DeviceResetInterface final
{
public:
    NTSTATUS Initialize(_In_ WDFDEVICE device) noexcept;
    NTSTATUS ResetDevice(_In_ ULONG resetType) noexcept;
    NTSTATUS ResetDeviceInPlace() noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsAvailable() const noexcept;
    [[nodiscard]] ULONG SupportedResetTypes() const noexcept;

private:
    void TraceResetStatus() const noexcept;
    UmDeviceResetInterface interface_;
    ULONGLONG pciResetTypes_;
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
    RegisterAperture() noexcept = default;
    ~RegisterAperture() noexcept { Reset(); }
    RegisterAperture(const RegisterAperture&) = delete;
    RegisterAperture& operator=(const RegisterAperture&) = delete;
    RegisterAperture(RegisterAperture&&) = delete;
    RegisterAperture& operator=(RegisterAperture&&) = delete;

    NTSTATUS InitializeRange(
        _In_ WDFDEVICE device,
        _In_ PHYSICAL_ADDRESS physicalAddress,
        _In_ SIZE_T length,
        _In_ MEMORY_CACHING_TYPE cacheType) noexcept;

    void Reset() noexcept;

    UCHAR ReadRegister8(_In_ SIZE_T offset) const noexcept;
    USHORT ReadRegister16(_In_ SIZE_T offset) const noexcept;
    ULONG ReadRegister32(_In_ SIZE_T offset) const noexcept;
    void ReadRegisterBuffer32(
        _In_ SIZE_T offset,
        _Out_writes_(count) ULONG* buffer,
        _In_ ULONG count) const noexcept;
    void WriteRegister8(_In_ SIZE_T offset, _In_ UCHAR value) const noexcept;
    void WriteRegister16(_In_ SIZE_T offset, _In_ USHORT value) const noexcept;
    void WriteRegister32(_In_ SIZE_T offset, _In_ ULONG value) const noexcept;
    NTSTATUS Read32(_In_ SIZE_T offset, _Out_ ULONG* value) const noexcept;

    [[nodiscard]] bool IsMapped() const noexcept;
    [[nodiscard]] volatile void* Address() const noexcept;
    [[nodiscard]] SIZE_T Length() const noexcept;
    [[nodiscard]] PHYSICAL_ADDRESS PhysicalAddress() const noexcept;
    [[nodiscard]] NV_PLATFORM_MMIO_ACCESS Access() noexcept;

private:
    static UCHAR ReadMmio8(void* context, SIZE_T offset);
    static USHORT ReadMmio16(void* context, SIZE_T offset);
    static ULONG ReadMmio32(void* context, SIZE_T offset);
    static void ReadMmioBuffer32(void* context, SIZE_T offset, ULONG* buffer, ULONG count);
    static void WriteMmio8(void* context, SIZE_T offset, UCHAR value);
    static void WriteMmio16(void* context, SIZE_T offset, USHORT value);
    static void WriteMmio32(void* context, SIZE_T offset, ULONG value);

    WDFDEVICE device_ = nullptr;
    void* pseudoAddress_ = nullptr;
    void* address_ = nullptr;
    SIZE_T length_ = 0;
    PHYSICAL_ADDRESS physicalAddress_ = {};
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
    static EVT_WDF_INTERRUPT_ENABLE Enable;
    static EVT_WDF_INTERRUPT_DISABLE Disable;

    WDFINTERRUPT handle_;
    ULONG vector_;
    ULONG count_;
};

} // namespace nv::iddcx