#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "device.hpp"
#include "trace.h"

#include "driver.tmh"

extern "C" DRIVER_INITIALIZE DriverEntry;

namespace {

EVT_WDF_OBJECT_CONTEXT_CLEANUP DriverCleanup;

_Use_decl_annotations_
void DriverCleanup(WDFOBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Driver cleanup");
    WPP_CLEANUP(WdfDriverWdmGetDriverObject(
        static_cast<WDFDRIVER>(driverObject)));
}

} // namespace

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
    WPP_INIT_TRACING(driverObject, registryPath);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DriverEntry");

    WDF_DRIVER_CONFIG configuration;
    WDF_DRIVER_CONFIG_INIT(&configuration, nv::iddcx::DeviceAdd);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = DriverCleanup;

    const NTSTATUS status = WdfDriverCreate(
        driverObject,
        registryPath,
        &attributes,
        &configuration,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "WdfDriverCreate failed: %!STATUS!",
            status);
        WPP_CLEANUP(driverObject);
    }
    return status;
}