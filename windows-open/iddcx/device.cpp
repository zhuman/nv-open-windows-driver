#include "device.hpp"

#include <new>

namespace nv::iddcx {

_Use_decl_annotations_
NTSTATUS DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    IDD_CX_CLIENT_CONFIG iddConfiguration;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfiguration);
    iddConfiguration.EvtIddCxAdapterInitFinished = AdapterInitFinished;
    iddConfiguration.EvtIddCxAdapterCommitModes = AdapterCommitModes;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfiguration);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = PrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = EnterD0;
    pnpCallbacks.EvtDeviceD0Exit = ExitD0;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = DeviceCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    void* contextStorage = GetDeviceContext(device);
    auto* context = new (contextStorage) DeviceContext{};

    const NV_STATUS nvStatus = nvCommonHostInitialize(
        &context->commonHost,
        GetCommonHostServices());
    if (nvStatus != NV_OK)
    {
        WdfObjectDelete(device);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    return IddCxDeviceInitialize(device);
}

_Use_decl_annotations_
void DeviceCleanup(WDFOBJECT deviceObject)
{
    DeviceContext* context = GetDeviceContext(deviceObject);
    context->adapter.Reset();
    nvCommonHostShutdown(&context->commonHost);
    context->~DeviceContext();
}

_Use_decl_annotations_
NTSTATUS PrepareHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesRaw,
    WDFCMRESLIST resourcesTranslated)
{
    DeviceContext* context = GetDeviceContext(device);
    const ULONG resourceCount = WdfCmResourceListGetCount(resourcesTranslated);

    if (resourceCount == 0)
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS status = context->configSpace.Initialize(device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = context->registerAperture.Initialize(
        device,
        resourcesTranslated,
        context->configSpace);
    if (!NT_SUCCESS(status))
    {
        context->configSpace.Reset();
        return status;
    }

    status = context->dmaEngine.Initialize(device);
    if (!NT_SUCCESS(status))
    {
        context->registerAperture.Reset();
        context->configSpace.Reset();
        return status;
    }

    status = context->interrupt.Initialize(
        device,
        resourcesRaw,
        resourcesTranslated);
    if (!NT_SUCCESS(status))
    {
        context->dmaEngine.Reset();
        context->registerAperture.Reset();
        context->configSpace.Reset();
        return status;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ReleaseHardware(
    WDFDEVICE device,
    WDFCMRESLIST resourcesTranslated)
{
    UNREFERENCED_PARAMETER(resourcesTranslated);

    DeviceContext* context = GetDeviceContext(device);
    context->interrupt.Reset();
    context->dmaEngine.Reset();
    context->registerAperture.Reset();
    context->configSpace.Reset();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EnterD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    UNREFERENCED_PARAMETER(previousState);
    return GetDeviceContext(device)->adapter.EnterD0(device);
}

_Use_decl_annotations_
NTSTATUS ExitD0(WDFDEVICE device, WDF_POWER_DEVICE_STATE targetState)
{
    UNREFERENCED_PARAMETER(targetState);
    GetDeviceContext(device)->adapter.ExitD0();
    return STATUS_SUCCESS;
}

} // namespace nv::iddcx