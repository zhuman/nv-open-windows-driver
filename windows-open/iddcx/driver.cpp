#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <new>

#include "device.hpp"
#include "driver_parameters.hpp"
#include "nvplatform_common.h"
#include "trace.h"

#include "driver.tmh"

extern "C" DRIVER_INITIALIZE DriverEntry;

namespace {

void PlatformDebugLog(NvU32 level, const char* message)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "NVRM platform level=%u: %s", level, message);
}

struct DriverContext
{
    bool initialized = false;
    nv::iddcx::DriverParameters parameters;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DriverContext, GetDriverContext);

EVT_WDF_OBJECT_CONTEXT_CLEANUP DriverCleanup;
EVT_WDF_OBJECT_CONTEXT_DESTROY DriverDestroy;

_Use_decl_annotations_
void DriverCleanup(WDFOBJECT driverObject)
{
    if (GetDriverContext(driverObject)->initialized)
        GetDriverContext(driverObject)->parameters.Stop();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Driver cleanup");
}

_Use_decl_annotations_
void DriverDestroy(WDFOBJECT driverObject)
{
    if (GetDriverContext(driverObject)->initialized)
    {
        GetDriverContext(driverObject)->~DriverContext();
        NvPlatformSetDebugLog(nullptr);
        WPP_CLEANUP(WdfDriverWdmGetDriverObject(static_cast<WDFDRIVER>(driverObject)));
    }
}

} // namespace

nv::iddcx::DriverParameters& nv::iddcx::Parameters() noexcept
{
    return GetDriverContext(WdfGetDriver())->parameters;
}

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
    NvPlatformSetDebugLog(PlatformDebugLog);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "DriverEntry");

    WDF_DRIVER_CONFIG configuration;
    WDF_DRIVER_CONFIG_INIT(&configuration, nv::iddcx::DeviceAdd);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DriverContext);
    attributes.EvtCleanupCallback = DriverCleanup;
    attributes.EvtDestroyCallback = DriverDestroy;

    WDFDRIVER driver = nullptr;
    NTSTATUS status = WdfDriverCreate(
        driverObject,
        registryPath,
        &attributes,
        &configuration,
        &driver);
    if (NT_SUCCESS(status))
    {
        auto* context = ::new (GetDriverContext(driver)) DriverContext{};
        context->initialized = true;
        status = context->parameters.Start();
        if (!NT_SUCCESS(status))
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Driver parameter watcher start failed: %!STATUS!", status);
        return status;
    }
    if (!NT_SUCCESS(status))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "WdfDriverCreate failed: %!STATUS!",
            status);
        NvPlatformSetDebugLog(nullptr);
        WPP_CLEANUP(driverObject);
    }
    return status;
}