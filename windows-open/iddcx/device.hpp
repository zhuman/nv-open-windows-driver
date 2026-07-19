#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "common_services.hpp"
#include "hardware_services.hpp"

namespace nv::iddcx {

struct DeviceContext final
{
    NV_COMMON_HOST_CONTEXT commonHost;
    ConfigSpace configSpace;
    RegisterAperture registerAperture;
    DmaEngine dmaEngine;
    Interrupt interrupt;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DeviceCleanup;

} // namespace nv::iddcx