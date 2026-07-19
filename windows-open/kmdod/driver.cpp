#include "ddi.hpp"
#include "driver.hpp"

#include "nvcommon_host.h"
#include "nvplatform_common.h"
#include "nvplatform_registry.h"
#include "nvplatform_system.h"
#include "nvport_windows.h"

namespace {

NV_COMMON_HOST_CONTEXT commonHost;
bool runtimeInitialized;

} // namespace

namespace nv::kmdod {

NTSTATUS InitializeDriverRuntime(
    _In_ PDRIVER_OBJECT driverObject,
    _In_ const UNICODE_STRING* registryPath) noexcept
{
    if (runtimeInitialized)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    NTSTATUS ntStatus = NvPlatformSystemInitialize(driverObject);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    ntStatus = NvPlatformRegistryInitialize(registryPath);
    if (!NT_SUCCESS(ntStatus))
    {
        NvPlatformSystemShutdown();
        return ntStatus;
    }

    NV_STATUS status = nvCommonHostInitialize(
        &commonHost,
        NvPlatformGetCommonHostServices());
    if (status != NV_OK)
    {
        NvPlatformRegistryShutdown();
        NvPlatformSystemShutdown();
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    status = nvPortWindowsInitialize(&commonHost);
    if (status != NV_OK)
    {
        nvCommonHostShutdown(&commonHost);
        NvPlatformRegistryShutdown();
        NvPlatformSystemShutdown();
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    runtimeInitialized = true;
    return STATUS_SUCCESS;
}

void ShutdownDriverRuntime() noexcept
{
    if (runtimeInitialized)
    {
        nvPortWindowsShutdown(&commonHost);
        nvCommonHostShutdown(&commonHost);
        NvPlatformRegistryShutdown();
        NvPlatformSystemShutdown();
        runtimeInitialized = false;
    }
}

} // namespace nv::kmdod

extern "C" DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT driverObject,
    PUNICODE_STRING registryPath)
{
    NTSTATUS status = nv::kmdod::InitializeDriverRuntime(driverObject, registryPath);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

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

    status = DxgkInitializeDisplayOnlyDriver(
        driverObject,
        registryPath,
        &initializationData);
    if (!NT_SUCCESS(status))
    {
        nv::kmdod::ShutdownDriverRuntime();
    }

    return status;
}
