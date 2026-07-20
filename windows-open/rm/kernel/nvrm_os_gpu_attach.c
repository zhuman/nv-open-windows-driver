#include "core/core.h"
#include "gpu/gpu.h"
#include "class/cl2080_notification.h"
#include "nverror.h"
#include "nvplatform_gpu_rm.h"

void osInitOSHwInfo(OBJGPU* pGpu)
{
    pGpu->pOsGpuInfo = NULL;
}

void osDestroyOSHwInfo(OBJGPU* pGpu)
{
    NV_ASSERT(pGpu->pOsGpuInfo == NULL);
}

NV_STATUS osDpcAttachGpu(OBJGPU* pGpu, void* pOsGpuInfo)
{
    NV_STATUS status = NvPlatformRmGpuReserve(pOsGpuInfo, pGpu);
    if (status == NV_OK)
    {
        pGpu->pOsGpuInfo = pOsGpuInfo;
    }
    return status;
}

NV_STATUS osAttachGpu(OBJGPU* pGpu, void* pOsGpuInfo)
{
    if (pGpu->pOsGpuInfo != pOsGpuInfo)
    {
        return NV_ERR_INVALID_STATE;
    }
    return NvPlatformRmGpuPublish(pOsGpuInfo, pGpu);
}

void osDpcDetachGpu(OBJGPU* pGpu)
{
    if (pGpu != NULL && pGpu->pOsGpuInfo != NULL)
    {
        NvPlatformRmGpuWithdraw(
            (NV_PLATFORM_RM_GPU_CONTEXT*)pGpu->pOsGpuInfo,
            pGpu);
        pGpu->pOsGpuInfo = NULL;
    }
}

NvBool osIsGpuAccessible(OBJGPU* pGpu)
{
    void* publishedGpu = NULL;
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext;

    if (pGpu == NULL || pGpu->pOsGpuInfo == NULL)
    {
        return NV_FALSE;
    }

    gpuContext = (NV_PLATFORM_RM_GPU_CONTEXT*)pGpu->pOsGpuInfo;
    return NvPlatformRmGpuGetPublishedRmGpu(gpuContext, &publishedGpu) == NV_OK &&
        publishedGpu == pGpu &&
        !NvPlatformRmGpuIsLost(gpuContext)
        ? NV_TRUE
        : NV_FALSE;
}

NV_STATUS osHandleGpuLost(OBJGPU* pGpu, NvBool bEmitXid)
{
    if (pGpu == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (!pGpu->getProperty(pGpu, PDB_PROP_GPU_IS_CONNECTED))
    {
        return NV_OK;
    }

    if (bEmitXid)
    {
        nvErrorLog_va(
            pGpu,
            ROBUST_CHANNEL_GPU_HAS_FALLEN_OFF_THE_BUS,
            "GPU has fallen off the bus.");
    }

    gpuNotifySubDeviceEvent(
        pGpu,
        NV2080_NOTIFIERS_GPU_UNAVAILABLE,
        NULL,
        0,
        ROBUST_CHANNEL_GPU_HAS_FALLEN_OFF_THE_BUS,
        0);
    gpuSetDisconnectedProperties(pGpu);
    NvPlatformRmGpuMarkLost(
        (NV_PLATFORM_RM_GPU_CONTEXT*)pGpu->pOsGpuInfo);
    return NV_OK;
}