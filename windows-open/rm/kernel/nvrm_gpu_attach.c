#include "core/core.h"
#include "gpu/gpu.h"
#include "gpu_mgr/gpu_mgr.h"
#include "rmapi/rmapi.h"
#include "nvrm_gpu_attach.h"

NV_STATUS NvRmAttachGpuContext(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32* gpuInstance,
    NvU32* gpuId)
{
    NV_PLATFORM_RM_GPU_ATTACH_INFO info;
    GPUATTACHARG attachArg = {0};
    NvU64 domainBusDevice;
    NV_STATUS status;

    if (gpuContext == NULL || gpuInstance == NULL || gpuId == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = NvPlatformRmGpuGetAttachInfo(gpuContext, &info);
    if (status != NV_OK || info.apertureCount == 0 ||
        info.apertures[0].kernelAddress == NULL ||
        info.apertures[0].length == 0 ||
        (info.apertureCount > 2 && info.apertures[2].length > NV_U32_MAX))
    {
        return status == NV_OK ? NV_ERR_INVALID_DATA : status;
    }

    domainBusDevice = gpuEncodeDomainBusDevice(info.domain, info.bus, info.device);
    *gpuId = gpuGenerate32BitId(info.domain, info.bus, info.device);

    attachArg.regBaseAddr = (GPUHWREG*)info.apertures[0].kernelAddress;
    attachArg.devPhysAddr = info.apertures[0].physicalAddress;
    attachArg.regLength = info.apertures[0].length;
    if (info.apertureCount > 1)
    {
        attachArg.fbPhysAddr = info.apertures[1].physicalAddress;
        attachArg.fbLength = info.apertures[1].length;
    }
    if (info.apertureCount > 2)
    {
        attachArg.instPhysAddr = info.apertures[2].physicalAddress;
        attachArg.instLength = (NvU32)info.apertures[2].length;
    }
    attachArg.nvDomainBusDeviceFunc = domainBusDevice;
    attachArg.intLine = info.interruptLine;
    attachArg.pOsAttachArg = gpuContext;
    attachArg.iovaspaceId = NV_U32_MAX;
    attachArg.cpuNumaNodeId = NV0000_CTRL_NO_NUMA_NODE;

    status = rmapiLockAcquire(RMAPI_LOCK_FLAGS_NONE, RM_LOCK_MODULES_INIT);
    if (status != NV_OK)
    {
        return status;
    }

    status = gpumgrRegisterGpuId(*gpuId, domainBusDevice);
    if (status == NV_OK)
    {
        status = gpumgrAllocGpuInstance(gpuInstance);
    }
    if (status == NV_OK)
    {
        status = gpumgrAttachGpu(*gpuInstance, &attachArg);
    }
    if (status != NV_OK)
    {
        (void)gpumgrUnregisterGpuId(*gpuId);
    }

    rmapiLockRelease();
    return status;
}

NV_STATUS NvRmDetachGpuContext(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32 gpuInstance,
    NvU32 gpuId)
{
    NV_STATUS status;

    if (gpuContext == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = rmapiLockAcquire(RMAPI_LOCK_FLAGS_NONE, RM_LOCK_MODULES_INIT);
    if (status != NV_OK)
    {
        return status;
    }

    status = gpumgrDetachGpu(gpuInstance);
    if (status == NV_OK)
    {
        status = gpumgrUnregisterGpuId(gpuId);
    }

    rmapiLockRelease();
    return status;
}