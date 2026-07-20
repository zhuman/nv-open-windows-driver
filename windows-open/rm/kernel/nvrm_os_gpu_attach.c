#include "core/core.h"
#include "gpu/gpu.h"
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