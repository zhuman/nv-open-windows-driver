#include "adapter.hpp"

void* __cdecl operator new(size_t, _In_ void* storage) noexcept
{
    return storage;
}

namespace {

constexpr ULONG AdapterPoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('A') << 16) |
    (static_cast<ULONG>('d') << 24);

} // namespace

namespace nv::kmdod {

Adapter::Adapter(_In_ DEVICE_OBJECT* physicalDeviceObject) noexcept
    : physicalDeviceObject_(physicalDeviceObject),
      startInfo_{},
      dxgkInterface_{},
      deviceInfo_{},
    hardwareResources_{},
    pciDevice_{},
    registerAperture_{},
            rmGpuContext_(nullptr),
      adapterPowerState_(PowerDeviceD0),
      state_(State::Stopped)
{
}

NTSTATUS Adapter::Create(
    _In_ DEVICE_OBJECT* physicalDeviceObject,
    _Outptr_ Adapter** adapter) noexcept
{
    if (physicalDeviceObject == nullptr || adapter == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *adapter = nullptr;
    void* storage = ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(Adapter),
        AdapterPoolTag);
    if (storage == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *adapter = new (storage) Adapter(physicalDeviceObject);
    return STATUS_SUCCESS;
}

void Adapter::Destroy(_In_opt_ Adapter* adapter) noexcept
{
    if (adapter == nullptr)
    {
        return;
    }

    adapter->Stop();
    adapter->~Adapter();
    ExFreePoolWithTag(adapter, AdapterPoolTag);
}

NTSTATUS Adapter::Start(
    _In_ DXGK_START_INFO* startInfo,
    _In_ DXGKRNL_INTERFACE* dxgkInterface,
    _Out_ ULONG* numberOfViews,
    _Out_ ULONG* numberOfChildren) noexcept
{
    if (startInfo == nullptr ||
        dxgkInterface == nullptr ||
        numberOfViews == nullptr ||
        numberOfChildren == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *numberOfViews = 0;
    *numberOfChildren = 0;

    if (state_ != State::Stopped)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    startInfo_ = *startInfo;
    dxgkInterface_ = *dxgkInterface;

    NTSTATUS status = dxgkInterface_.DxgkCbGetDeviceInformation(
        dxgkInterface_.DeviceHandle,
        &deviceInfo_);
    if (!NT_SUCCESS(status))
    {
        ClearRuntimeState();
        return status;
    }

    status = hardwareResources_.Initialize(deviceInfo_.TranslatedResourceList);
    if (!NT_SUCCESS(status))
    {
        ClearRuntimeState();
        return status;
    }

    status = pciDevice_.Initialize(
        physicalDeviceObject_,
        dxgkInterface_,
        hardwareResources_.MemoryRangeCount() != 0);
    if (!NT_SUCCESS(status))
    {
        ClearRuntimeState();
        return status;
    }

    status = registerAperture_.Initialize(hardwareResources_, pciDevice_);
    if (!NT_SUCCESS(status))
    {
        ClearRuntimeState();
        return status;
    }

    NV_PLATFORM_RM_GPU_ATTACH_INFO attachInfo = {};
    const NV_PLATFORM_PCI_LOCATION* location = pciDevice_.Location();
    if (location == nullptr)
    {
        ClearRuntimeState();
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    attachInfo.domain = location->domain;
    attachInfo.bus = location->bus;
    attachInfo.device = location->device;
    attachInfo.function = location->function;
    attachInfo.apertureCount = static_cast<NvU8>(pciDevice_.MemoryBarCount());
    for (ULONG barIndex = 0; barIndex < pciDevice_.MemoryBarCount(); ++barIndex)
    {
        const PHYSICAL_ADDRESS barAddress = pciDevice_.MemoryBarAddress(barIndex);
        const MemoryRange* barRange = nullptr;
        for (ULONG rangeIndex = 0; rangeIndex < hardwareResources_.MemoryRangeCount(); ++rangeIndex)
        {
            const MemoryRange* candidate = hardwareResources_.MemoryRangeAt(rangeIndex);
            if (candidate != nullptr &&
                candidate->physicalAddress.QuadPart == barAddress.QuadPart)
            {
                barRange = candidate;
                break;
            }
        }

        if (barRange == nullptr)
        {
            ClearRuntimeState();
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        attachInfo.apertures[barIndex].physicalAddress =
            static_cast<NvU64>(barRange->physicalAddress.QuadPart);
        attachInfo.apertures[barIndex].length = static_cast<NvU64>(barRange->length);
    }
    attachInfo.apertures[0].kernelAddress = registerAperture_.Address();
    attachInfo.interruptCount = hardwareResources_.InterruptCount();
    if (const InterruptResource* interrupt = hardwareResources_.InterruptAt(0))
    {
        attachInfo.interruptLine = interrupt->vector;
    }

    const NV_STATUS rmStatus = NvPlatformRmGpuContextCreate(
        this,
        &attachInfo,
        &rmGpuContext_);
    if (rmStatus != NV_OK)
    {
        ClearRuntimeState();
        return rmStatus == NV_ERR_NO_MEMORY
            ? STATUS_INSUFFICIENT_RESOURCES
            : STATUS_UNSUCCESSFUL;
    }

    adapterPowerState_ = PowerDeviceD0;
    state_ = State::Started;

    *numberOfViews = 1;
    *numberOfChildren = 0;
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::Stop() noexcept
{
    if (state_ == State::Stopped)
    {
        return STATUS_SUCCESS;
    }

    // StopDevice is terminal for this adapter context. Ordinary D-state
    // transitions are reported through SetPowerState and do not set this bit.
    NvPlatformRmGpuMarkShutdown(rmGpuContext_);
    ClearRuntimeState();
    return STATUS_SUCCESS;
}

void Adapter::Reset() noexcept
{
    if (state_ == State::Started)
    {
        adapterPowerState_ = PowerDeviceD0;
    }
}

BOOLEAN Adapter::InterruptRoutine(_In_ ULONG messageNumber) noexcept
{
    if (state_ != State::Started)
    {
        return FALSE;
    }

    UNREFERENCED_PARAMETER(messageNumber);
    return FALSE;
}

void Adapter::DpcRoutine() noexcept
{
}

NTSTATUS Adapter::DispatchIoRequest(
    _In_ ULONG vidPnSourceId,
    _In_ VIDEO_REQUEST_PACKET* request) const noexcept
{
    UNREFERENCED_PARAMETER(vidPnSourceId);
    UNREFERENCED_PARAMETER(request);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS Adapter::SetPowerState(
    _In_ ULONG hardwareUid,
    _In_ DEVICE_POWER_STATE devicePowerState,
    _In_ POWER_ACTION actionType) noexcept
{
    UNREFERENCED_PARAMETER(actionType);

    if (hardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        adapterPowerState_ = devicePowerState;
    }

    return STATUS_SUCCESS;
}

NTSTATUS Adapter::QueryChildRelations(
    _Out_writes_bytes_(childRelationsSize) DXGK_CHILD_DESCRIPTOR* childRelations,
    _In_ ULONG childRelationsSize) const noexcept
{
    if (childRelations == nullptr || childRelationsSize < sizeof(DXGK_CHILD_DESCRIPTOR))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(childRelations, childRelationsSize);
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::QueryChildStatus(
    _Inout_ DXGK_CHILD_STATUS* childStatus,
    _In_ BOOLEAN nonDestructiveOnly) const noexcept
{
    UNREFERENCED_PARAMETER(childStatus);
    UNREFERENCED_PARAMETER(nonDestructiveOnly);
    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

NTSTATUS Adapter::QueryDeviceDescriptor(
    _In_ ULONG childUid,
    _Inout_ DXGK_DEVICE_DESCRIPTOR* deviceDescriptor) const noexcept
{
    UNREFERENCED_PARAMETER(childUid);
    UNREFERENCED_PARAMETER(deviceDescriptor);
    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

NTSTATUS Adapter::QueryAdapterInfo(
    _In_ const DXGKARG_QUERYADAPTERINFO* queryAdapterInfo) const noexcept
{
    if (queryAdapterInfo == nullptr || queryAdapterInfo->pOutputData == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (queryAdapterInfo->Type != DXGKQAITYPE_DRIVERCAPS)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (queryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    auto* capabilities = static_cast<DXGK_DRIVERCAPS*>(queryAdapterInfo->pOutputData);
    RtlZeroMemory(capabilities, sizeof(*capabilities));
    capabilities->WDDMVersion = DXGKDDI_WDDMv1_2;
    capabilities->HighestAcceptableAddress.QuadPart = -1;
    capabilities->SupportNonVGA = TRUE;
    return STATUS_SUCCESS;
}

bool Adapter::IsStarted() const noexcept
{
    return state_ == State::Started;
}

NV_PLATFORM_RM_GPU_CONTEXT* Adapter::RmGpuContext() const noexcept
{
    return state_ == State::Started ? rmGpuContext_ : nullptr;
}

void Adapter::ClearRuntimeState() noexcept
{
    NvPlatformRmGpuContextDestroy(rmGpuContext_);
    rmGpuContext_ = nullptr;
    registerAperture_.Reset();
    pciDevice_.Reset();
    hardwareResources_.Reset();
    RtlZeroMemory(&startInfo_, sizeof(startInfo_));
    RtlZeroMemory(&dxgkInterface_, sizeof(dxgkInterface_));
    RtlZeroMemory(&deviceInfo_, sizeof(deviceInfo_));
    adapterPowerState_ = PowerDeviceD3;
    state_ = State::Stopped;
}

} // namespace nv::kmdod