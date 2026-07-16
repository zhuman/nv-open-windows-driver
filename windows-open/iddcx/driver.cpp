#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD NvIddCxDeviceAdd;

extern "C" BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID reserved)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(reserved);
    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT driverObject,
    PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG configuration;
    WDF_DRIVER_CONFIG_INIT(&configuration, NvIddCxDeviceAdd);

    return WdfDriverCreate(
        driverObject,
        registryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configuration,
        WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS NvIddCxDeviceAdd(
    WDFDRIVER driver,
    PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    IDD_CX_CLIENT_CONFIG iddConfiguration;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfiguration);

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfiguration);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    return IddCxDeviceInitialize(device);
}