#include <ntddk.h>

extern "C" {
#include <dispmprt.h>
}

extern "C" DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT driverObject,
    PUNICODE_STRING registryPath)
{
    KMDDOD_INITIALIZATION_DATA initializationData = {};
    initializationData.Version = DXGKDDI_INTERFACE_VERSION;

    return DxgkInitializeDisplayOnlyDriver(
        driverObject,
        registryPath,
        &initializationData);
}