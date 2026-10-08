#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "adapter.hpp"
#include "common_services.hpp"
#include "hardware_services.hpp"
#include "pci_bars.hpp"
#include "nvplatform_gpu_rm.h"
#include "nvplatform_memory_user.h"
#include "nvplatform_mmio_user.h"
#include "nvplatform_pci.h"
#include "nvport_windows.h"

namespace nv::iddcx {

struct DeviceContext final
{
    NV_COMMON_HOST_CONTEXT commonHost;
    Adapter adapter;
    DeviceResetInterface deviceReset;
    ConfigSpace configSpace;
    void* pciConfigRegistration;
    PciBars bars;
    DmaEngine dmaEngine;
    void* dmaRegistration;
    Interrupt interrupt;
    NV_PLATFORM_RM_GPU_CONTEXT* rmGpuContext;
    NvU32 rmGpuInstance;
    NvU32 rmDeviceInstance;
    NvU32 rmGpuId;
    NvHandle rmClientHandle;
    NvHandle rmDeviceHandle;
    NvHandle rmSubdeviceHandle;
    HANDLE nvKmsThread;
    WDFWORKITEM displayChangeWorkItem;
    SRWLOCK lifecycleLock;
    volatile LONG nvKmsWorkCanceled;
    volatile LONG nvKmsWorkCompleted;
    volatile LONG d0Active;
    bool rmDeviceOpened;
    bool rmGpuAttached;
    bool rmGpuStateLoaded;
    bool nvKmsInitialized;
    bool rmInitialized;
    bool nvPortInitialized;
    bool d3d12SwapChainSupported;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EnterD0;
EVT_WDF_DEVICE_D0_EXIT ExitD0;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DeviceCleanup;
NTSTATUS StartNvKmsInitialization(_In_ WDFDEVICE device) noexcept;
DWORD WINAPI InitializeNvKms(_In_ void* parameter);
EVT_WDF_WORKITEM DisplayChangeWorkItem;

} // namespace nv::iddcx