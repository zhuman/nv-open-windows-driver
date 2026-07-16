#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "hardware_services.hpp"

namespace nv::iddcx {

struct DeviceContext final
{
    ConfigSpace configSpace;
    DmaEngine dmaEngine;
    Interrupt interrupt;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;

} // namespace nv::iddcx