#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "device.hpp"

extern "C" DRIVER_INITIALIZE DriverEntry;

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
    WDF_DRIVER_CONFIG_INIT(&configuration, nv::iddcx::DeviceAdd);

    return WdfDriverCreate(
        driverObject,
        registryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configuration,
        WDF_NO_HANDLE);
}