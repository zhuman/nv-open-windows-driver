#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NV_PLATFORM_RM_GPU_CONTEXT NV_PLATFORM_RM_GPU_CONTEXT;

NV_STATUS NvPlatformRmGpuContextCreate(
    void* adapterContext,
    NV_PLATFORM_RM_GPU_CONTEXT** gpuContext);
void NvPlatformRmGpuContextDestroy(NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
NV_STATUS NvPlatformRmGpuReserve(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu);
NV_STATUS NvPlatformRmGpuPublish(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu);
void NvPlatformRmGpuWithdraw(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu);

#ifdef __cplusplus
}
#endif