#pragma once

#include "hardware_resources.hpp"
#include "pci_device.hpp"
#include "wdk.hpp"

namespace nv::kmdod {

class Adapter final
{
public:
    static NTSTATUS Create(
        _In_ DEVICE_OBJECT* physicalDeviceObject,
        _Outptr_ Adapter** adapter) noexcept;

    static void Destroy(_In_opt_ Adapter* adapter) noexcept;

    NTSTATUS Start(
        _In_ DXGK_START_INFO* startInfo,
        _In_ DXGKRNL_INTERFACE* dxgkInterface,
        _Out_ ULONG* numberOfViews,
        _Out_ ULONG* numberOfChildren) noexcept;

    NTSTATUS Stop() noexcept;
    void Reset() noexcept;

    NTSTATUS DispatchIoRequest(
        _In_ ULONG vidPnSourceId,
        _In_ VIDEO_REQUEST_PACKET* request) const noexcept;

    NTSTATUS SetPowerState(
        _In_ ULONG hardwareUid,
        _In_ DEVICE_POWER_STATE devicePowerState,
        _In_ POWER_ACTION actionType) noexcept;

    NTSTATUS QueryChildRelations(
        _Out_writes_bytes_(childRelationsSize) DXGK_CHILD_DESCRIPTOR* childRelations,
        _In_ ULONG childRelationsSize) const noexcept;

    NTSTATUS QueryChildStatus(
        _Inout_ DXGK_CHILD_STATUS* childStatus,
        _In_ BOOLEAN nonDestructiveOnly) const noexcept;

    NTSTATUS QueryDeviceDescriptor(
        _In_ ULONG childUid,
        _Inout_ DXGK_DEVICE_DESCRIPTOR* deviceDescriptor) const noexcept;

    NTSTATUS QueryAdapterInfo(
        _In_ const DXGKARG_QUERYADAPTERINFO* queryAdapterInfo) const noexcept;

    [[nodiscard]] bool IsStarted() const noexcept;

private:
    enum class State : UCHAR
    {
        Stopped,
        Started,
    };

    explicit Adapter(_In_ DEVICE_OBJECT* physicalDeviceObject) noexcept;
    ~Adapter() = default;

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    void ClearRuntimeState() noexcept;

    DEVICE_OBJECT* physicalDeviceObject_;
    DXGK_START_INFO startInfo_;
    DXGKRNL_INTERFACE dxgkInterface_;
    DXGK_DEVICE_INFO deviceInfo_;
    HardwareResources hardwareResources_;
    PciDevice pciDevice_;
    DEVICE_POWER_STATE adapterPowerState_;
    State state_;
};

} // namespace nv::kmdod