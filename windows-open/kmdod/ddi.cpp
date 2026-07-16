#include "ddi.hpp"

#include "adapter.hpp"

namespace {

nv::kmdod::Adapter* GetAdapter(_In_opt_ void* deviceContext) noexcept
{
    return static_cast<nv::kmdod::Adapter*>(deviceContext);
}

} // namespace

namespace nv::kmdod::ddi {

_Use_decl_annotations_
NTSTATUS AddDevice(
    DEVICE_OBJECT* physicalDeviceObject,
    void** deviceContext)
{
    PAGED_CODE();
    return Adapter::Create(
        physicalDeviceObject,
        reinterpret_cast<Adapter**>(deviceContext));
}

_Use_decl_annotations_
NTSTATUS StartDevice(
    void* deviceContext,
    DXGK_START_INFO* startInfo,
    DXGKRNL_INTERFACE* dxgkInterface,
    ULONG* numberOfViews,
    ULONG* numberOfChildren)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    if (adapter == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return adapter->Start(
        startInfo,
        dxgkInterface,
        numberOfViews,
        numberOfChildren);
}

_Use_decl_annotations_
NTSTATUS StopDevice(void* deviceContext)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr ? STATUS_INVALID_PARAMETER : adapter->Stop();
}

_Use_decl_annotations_
NTSTATUS RemoveDevice(void* deviceContext)
{
    PAGED_CODE();
    Adapter::Destroy(GetAdapter(deviceContext));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void ResetDevice(void* deviceContext)
{
    Adapter* adapter = GetAdapter(deviceContext);
    if (adapter != nullptr)
    {
        adapter->Reset();
    }
}

_Use_decl_annotations_
void Unload()
{
    PAGED_CODE();
}

_Use_decl_annotations_
NTSTATUS DispatchIoRequest(
    void* deviceContext,
    ULONG vidPnSourceId,
    VIDEO_REQUEST_PACKET* request)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr
        ? STATUS_INVALID_PARAMETER
        : adapter->DispatchIoRequest(vidPnSourceId, request);
}

_Use_decl_annotations_
BOOLEAN InterruptRoutine(void* deviceContext, ULONG messageNumber)
{
    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr ? FALSE : adapter->InterruptRoutine(messageNumber);
}

_Use_decl_annotations_
void DpcRoutine(void* deviceContext)
{
    Adapter* adapter = GetAdapter(deviceContext);
    if (adapter != nullptr)
    {
        adapter->DpcRoutine();
    }
}

_Use_decl_annotations_
NTSTATUS QueryChildRelations(
    void* deviceContext,
    DXGK_CHILD_DESCRIPTOR* childRelations,
    ULONG childRelationsSize)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr
        ? STATUS_INVALID_PARAMETER
        : adapter->QueryChildRelations(childRelations, childRelationsSize);
}

_Use_decl_annotations_
NTSTATUS QueryChildStatus(
    void* deviceContext,
    DXGK_CHILD_STATUS* childStatus,
    BOOLEAN nonDestructiveOnly)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr
        ? STATUS_INVALID_PARAMETER
        : adapter->QueryChildStatus(childStatus, nonDestructiveOnly);
}

_Use_decl_annotations_
NTSTATUS QueryDeviceDescriptor(
    void* deviceContext,
    ULONG childUid,
    DXGK_DEVICE_DESCRIPTOR* deviceDescriptor)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr
        ? STATUS_INVALID_PARAMETER
        : adapter->QueryDeviceDescriptor(childUid, deviceDescriptor);
}

_Use_decl_annotations_
NTSTATUS SetPowerState(
    void* deviceContext,
    ULONG hardwareUid,
    DEVICE_POWER_STATE devicePowerState,
    POWER_ACTION actionType)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr
        ? STATUS_INVALID_PARAMETER
        : adapter->SetPowerState(hardwareUid, devicePowerState, actionType);
}

_Use_decl_annotations_
NTSTATUS APIENTRY QueryAdapterInfo(
    const HANDLE adapterHandle,
    const DXGKARG_QUERYADAPTERINFO* queryAdapterInfo)
{
    PAGED_CODE();

    Adapter* adapter = GetAdapter(adapterHandle);
    return adapter == nullptr
        ? STATUS_INVALID_PARAMETER
        : adapter->QueryAdapterInfo(queryAdapterInfo);
}

_Use_decl_annotations_
NTSTATUS APIENTRY SetPointerPosition(
    const HANDLE adapterHandle,
    const DXGKARG_SETPOINTERPOSITION* pointerPosition)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(pointerPosition);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY SetPointerShape(
    const HANDLE adapterHandle,
    const DXGKARG_SETPOINTERSHAPE* pointerShape)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(pointerShape);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY PresentDisplayOnly(
    const HANDLE adapterHandle,
    const DXGKARG_PRESENT_DISPLAYONLY* present)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(present);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY IsSupportedVidPn(
    const HANDLE adapterHandle,
    DXGKARG_ISSUPPORTEDVIDPN* isSupportedVidPn)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);

    if (isSupportedVidPn == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    isSupportedVidPn->IsVidPnSupported = FALSE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS APIENTRY RecommendFunctionalVidPn(
    const HANDLE adapterHandle,
    const DXGKARG_RECOMMENDFUNCTIONALVIDPN* recommendFunctionalVidPn)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(recommendFunctionalVidPn);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY EnumVidPnCofuncModality(
    const HANDLE adapterHandle,
    const DXGKARG_ENUMVIDPNCOFUNCMODALITY* enumCofuncModality)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(enumCofuncModality);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY SetVidPnSourceVisibility(
    const HANDLE adapterHandle,
    const DXGKARG_SETVIDPNSOURCEVISIBILITY* sourceVisibility)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(sourceVisibility);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY CommitVidPn(
    const HANDLE adapterHandle,
    const DXGKARG_COMMITVIDPN* commitVidPn)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(commitVidPn);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY UpdateActiveVidPnPresentPath(
    const HANDLE adapterHandle,
    const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* updatePath)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(updatePath);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY RecommendMonitorModes(
    const HANDLE adapterHandle,
    const DXGKARG_RECOMMENDMONITORMODES* recommendMonitorModes)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);
    UNREFERENCED_PARAMETER(recommendMonitorModes);
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS APIENTRY QueryVidPnHardwareCapability(
    const HANDLE adapterHandle,
    DXGKARG_QUERYVIDPNHWCAPABILITY* hardwareCapability)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(adapterHandle);

    if (hardwareCapability == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(
        &hardwareCapability->VidPnHWCaps,
        sizeof(hardwareCapability->VidPnHWCaps));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS StopDeviceAndReleasePostDisplayOwnership(
    void* deviceContext,
    D3DDDI_VIDEO_PRESENT_TARGET_ID targetId,
    DXGK_DISPLAY_INFORMATION* displayInformation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(targetId);

    if (displayInformation == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(displayInformation, sizeof(*displayInformation));

    Adapter* adapter = GetAdapter(deviceContext);
    return adapter == nullptr ? STATUS_INVALID_PARAMETER : adapter->Stop();
}

_Use_decl_annotations_
NTSTATUS SystemDisplayEnable(
    void* deviceContext,
    D3DDDI_VIDEO_PRESENT_TARGET_ID targetId,
    DXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS* flags,
    UINT* width,
    UINT* height,
    D3DDDIFORMAT* colorFormat)
{
    UNREFERENCED_PARAMETER(deviceContext);
    UNREFERENCED_PARAMETER(targetId);
    UNREFERENCED_PARAMETER(flags);

    if (width == nullptr || height == nullptr || colorFormat == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *width = 0;
    *height = 0;
    *colorFormat = D3DDDIFMT_UNKNOWN;
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
void SystemDisplayWrite(
    void* deviceContext,
    void* source,
    UINT sourceWidth,
    UINT sourceHeight,
    UINT sourceStride,
    UINT positionX,
    UINT positionY)
{
    UNREFERENCED_PARAMETER(deviceContext);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(sourceWidth);
    UNREFERENCED_PARAMETER(sourceHeight);
    UNREFERENCED_PARAMETER(sourceStride);
    UNREFERENCED_PARAMETER(positionX);
    UNREFERENCED_PARAMETER(positionY);
}

} // namespace nv::kmdod::ddi