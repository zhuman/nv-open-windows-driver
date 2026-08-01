#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NV_PLATFORM_RM_GPU_CONTEXT NV_PLATFORM_RM_GPU_CONTEXT;

typedef struct NV_PLATFORM_RM_GPU_APERTURE
{
    NvU64 physicalAddress;
    volatile void* kernelAddress;
    NvU64 length;
} NV_PLATFORM_RM_GPU_APERTURE;

typedef struct NV_PLATFORM_RM_GPU_ATTACH_INFO
{
    NvU32 domain;
    NvU8 bus;
    NvU8 device;
    NvU8 function;
    NvU8 apertureCount;
    NV_PLATFORM_RM_GPU_APERTURE apertures[3];
    NvU32 interruptLine;
    NvU32 interruptCount;
} NV_PLATFORM_RM_GPU_ATTACH_INFO;

NV_STATUS NvPlatformRmGpuContextCreate(
    void* adapterContext,
    const NV_PLATFORM_RM_GPU_ATTACH_INFO* attachInfo,
    NV_PLATFORM_RM_GPU_CONTEXT** gpuContext);
void NvPlatformRmGpuContextDestroy(NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
NV_STATUS NvPlatformRmGpuGetAttachInfo(
    const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NV_PLATFORM_RM_GPU_ATTACH_INFO* attachInfo);
NV_STATUS NvPlatformRmGpuReserve(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu);
NV_STATUS NvPlatformRmGpuPublish(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu);
// Resolves OBJGPU only after the WDDM adapter context has completed RM attach.
NV_STATUS NvPlatformRmGpuGetPublishedRmGpu(
    const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void** rmGpu);
void NvPlatformRmGpuWithdraw(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu);
void NvPlatformRmGpuMarkLost(NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
NvBool NvPlatformRmGpuIsLost(const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
// Records terminal WDDM adapter stop separately from recoverable D-state changes.
void NvPlatformRmGpuMarkShutdown(NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
NvBool NvPlatformRmGpuIsShutdown(const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
void NvPlatformRmGpuRequireClientCleanup(NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
NvBool NvPlatformRmGpuNeedsClientCleanup(const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);
void NvPlatformRmGpuCompleteClientCleanup(NV_PLATFORM_RM_GPU_CONTEXT* gpuContext);

#ifdef __cplusplus
}
#endif