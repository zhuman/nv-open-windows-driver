#pragma once

#include "nvplatform_gpu_rm.h"
#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvRmAttachGpuContext(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32* gpuInstance,
    NvU32* deviceInstance,
    NvU32* gpuId);

NV_STATUS NvRmDetachGpuContext(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32 gpuInstance,
    NvU32 deviceInstance,
    NvU32 gpuId);

typedef enum NV_RM_GPU_OPEN_STAGE
{
    NV_RM_GPU_OPEN_STAGE_ROOT_CLIENT,
    NV_RM_GPU_OPEN_STAGE_GPU_ID,
    NV_RM_GPU_OPEN_STAGE_DEVICE,
    NV_RM_GPU_OPEN_STAGE_SUBDEVICE_COUNT,
    NV_RM_GPU_OPEN_STAGE_SUBDEVICE,
    NV_RM_GPU_OPEN_STAGE_COMPLETE,
} NV_RM_GPU_OPEN_STAGE;

NV_STATUS NvRmOpenGpuDevice(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32 gpuId,
    NvU32 expectedDeviceInstance,
    NvHandle* clientHandle,
    NvHandle* deviceHandle,
    NvHandle* subdeviceHandle,
    NV_RM_GPU_OPEN_STAGE* openStage);

NV_STATUS NvRmCloseGpuDevice(
    NvHandle clientHandle);

NV_STATUS NvRmCleanupGpuClients(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);

#ifdef __cplusplus
}
#endif