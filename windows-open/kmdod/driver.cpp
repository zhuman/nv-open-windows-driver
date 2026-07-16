#include "ddi.hpp"

extern "C" DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT driverObject,
    PUNICODE_STRING registryPath)
{
    KMDDOD_INITIALIZATION_DATA initializationData = {};
    initializationData.Version = DXGKDDI_INTERFACE_VERSION;
    initializationData.DxgkDdiAddDevice = nv::kmdod::ddi::AddDevice;
    initializationData.DxgkDdiStartDevice = nv::kmdod::ddi::StartDevice;
    initializationData.DxgkDdiStopDevice = nv::kmdod::ddi::StopDevice;
    initializationData.DxgkDdiRemoveDevice = nv::kmdod::ddi::RemoveDevice;
    initializationData.DxgkDdiResetDevice = nv::kmdod::ddi::ResetDevice;
    initializationData.DxgkDdiUnload = nv::kmdod::ddi::Unload;
    initializationData.DxgkDdiDispatchIoRequest = nv::kmdod::ddi::DispatchIoRequest;
    initializationData.DxgkDdiInterruptRoutine = nv::kmdod::ddi::InterruptRoutine;
    initializationData.DxgkDdiDpcRoutine = nv::kmdod::ddi::DpcRoutine;
    initializationData.DxgkDdiQueryChildRelations = nv::kmdod::ddi::QueryChildRelations;
    initializationData.DxgkDdiQueryChildStatus = nv::kmdod::ddi::QueryChildStatus;
    initializationData.DxgkDdiQueryDeviceDescriptor = nv::kmdod::ddi::QueryDeviceDescriptor;
    initializationData.DxgkDdiSetPowerState = nv::kmdod::ddi::SetPowerState;
    initializationData.DxgkDdiQueryAdapterInfo = nv::kmdod::ddi::QueryAdapterInfo;
    initializationData.DxgkDdiSetPointerPosition = nv::kmdod::ddi::SetPointerPosition;
    initializationData.DxgkDdiSetPointerShape = nv::kmdod::ddi::SetPointerShape;
    initializationData.DxgkDdiPresentDisplayOnly = nv::kmdod::ddi::PresentDisplayOnly;
    initializationData.DxgkDdiIsSupportedVidPn = nv::kmdod::ddi::IsSupportedVidPn;
    initializationData.DxgkDdiRecommendFunctionalVidPn = nv::kmdod::ddi::RecommendFunctionalVidPn;
    initializationData.DxgkDdiEnumVidPnCofuncModality = nv::kmdod::ddi::EnumVidPnCofuncModality;
    initializationData.DxgkDdiSetVidPnSourceVisibility = nv::kmdod::ddi::SetVidPnSourceVisibility;
    initializationData.DxgkDdiCommitVidPn = nv::kmdod::ddi::CommitVidPn;
    initializationData.DxgkDdiUpdateActiveVidPnPresentPath = nv::kmdod::ddi::UpdateActiveVidPnPresentPath;
    initializationData.DxgkDdiRecommendMonitorModes = nv::kmdod::ddi::RecommendMonitorModes;
    initializationData.DxgkDdiQueryVidPnHWCapability = nv::kmdod::ddi::QueryVidPnHardwareCapability;
    initializationData.DxgkDdiStopDeviceAndReleasePostDisplayOwnership =
        nv::kmdod::ddi::StopDeviceAndReleasePostDisplayOwnership;
    initializationData.DxgkDdiSystemDisplayEnable = nv::kmdod::ddi::SystemDisplayEnable;
    initializationData.DxgkDdiSystemDisplayWrite = nv::kmdod::ddi::SystemDisplayWrite;

    return DxgkInitializeDisplayOnlyDriver(
        driverObject,
        registryPath,
        &initializationData);
}