#include "device.hpp"

#include <new>

namespace nv::iddcx {

_Use_decl_annotations_
NTSTATUS DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    IDD_CX_CLIENT_CONFIG iddConfiguration;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfiguration);

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfiguration);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = PrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    void* contextStorage = GetDeviceContext(device);
    new (contextStorage) DeviceContext{};

    return IddCxDeviceInitialize(device);
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

    status = context->dmaEngine.Initialize(device);
    if (!NT_SUCCESS(status))
    {
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
    context->configSpace.Reset();
    return STATUS_SUCCESS;
}

} // namespace nv::iddcx