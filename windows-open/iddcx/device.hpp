#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "adapter.hpp"
#include "common_services.hpp"
#include "hardware_services.hpp"

namespace nv::iddcx {

struct DeviceContext final
{
    NV_COMMON_HOST_CONTEXT commonHost;
    Adapter adapter;
    ConfigSpace configSpace;
    RegisterAperture registerAperture;
    DmaEngine dmaEngine;
    Interrupt interrupt;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EnterD0;
EVT_WDF_DEVICE_D0_EXIT ExitD0;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DeviceCleanup;

} // namespace nv::iddcx