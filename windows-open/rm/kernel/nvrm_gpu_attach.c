#include "core/core.h"
#include "core/locks.h"
#include "gpu/gpu.h"
#include "gpu_mgr/gpu_mgr.h"
#include "rmapi/rmapi.h"
#include "g_rs_server_nvoc.h"
#include "nvrm_gpu_attach.h"

#include <class/cl0000.h>
#include <class/cl0080.h>
#include <class/cl2080.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl0080/ctrl0080gpu.h>

NV_STATUS NvRmAttachGpuContext(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32* gpuInstance,
    NvU32* deviceInstance,
    NvU32* gpuId)
{
    NV_PLATFORM_RM_GPU_ATTACH_INFO info;
    GPUATTACHARG attachArg = {0};
    NvU64 domainBusDevice;
    NV_STATUS status;

    if (gpuContext == NULL || gpuInstance == NULL ||
        deviceInstance == NULL || gpuId == NULL)
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
    attachArg.bRequestFwClientRm = NV_TRUE;

    status = rmapiLockAcquire(RMAPI_LOCK_FLAGS_NONE, RM_LOCK_MODULES_INIT);
    if (status != NV_OK)
    {
        return status;
    }

    status = gpumgrThreadEnableExpandedGpuVisibility();
    if (status != NV_OK)
    {
        rmapiLockRelease();
        return status;
    }

    status = gpumgrRegisterGpuId(*gpuId, domainBusDevice);
    if (status == NV_OK)
    {
        status = gpumgrAllocGpuInstance(gpuInstance);
    }
    if (status == NV_OK)
    {
        status = rmGpuLockAlloc(*gpuInstance);
    }
    if (status == NV_OK)
    {
        status = gpumgrCreateDevice(
            deviceInstance,
            NVBIT(*gpuInstance),
            NULL);
        if (status != NV_OK)
        {
            rmGpuLockFree(*gpuInstance);
        }
    }
    if (status == NV_OK)
    {
        status = gpumgrAttachGpu(*gpuInstance, &attachArg);
        if (status != NV_OK)
        {
            gpumgrDestroyDevice(*deviceInstance);
            rmGpuLockFree(*gpuInstance);
        }
    }
    if (status != NV_OK)
    {
        (void)gpumgrUnregisterGpuId(*gpuId);
    }

    gpumgrThreadDisableExpandedGpuVisibility();
    rmapiLockRelease();
    return status;
}

NV_STATUS NvRmDetachGpuContext(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32 gpuInstance,
    NvU32 deviceInstance,
    NvU32 gpuId)
{
    NV_STATUS status;

    if (gpuContext == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NvPlatformRmGpuMarkShutdown(gpuContext);

    status = NvRmCleanupGpuClients(gpuContext);
    if (status != NV_OK)
    {
        return status;
    }

    status = rmapiLockAcquire(RMAPI_LOCK_FLAGS_NONE, RM_LOCK_MODULES_INIT);
    if (status != NV_OK)
    {
        return status;
    }

    status = gpumgrThreadEnableExpandedGpuVisibility();
    if (status != NV_OK)
    {
        rmapiLockRelease();
        return status;
    }

    status = gpumgrDetachGpu(gpuInstance);
    if (status == NV_OK)
    {
        gpumgrDestroyDevice(deviceInstance);
        rmGpuLockFree(gpuInstance);
        status = gpumgrUnregisterGpuId(gpuId);
    }

    gpumgrThreadDisableExpandedGpuVisibility();
    rmapiLockRelease();
    return status;
}

NV_STATUS NvRmOpenGpuDevice(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NvU32 gpuId,
    NvU32 expectedDeviceInstance,
    NvHandle* clientHandle,
    NvHandle* deviceHandle,
    NvHandle* subdeviceHandle,
    NV_RM_GPU_OPEN_STAGE* openStage)
{
    API_SECURITY_INFO securityInfo = {0};
    NvHandle allocatedClientHandle = NV01_NULL_OBJECT;
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS idInfoParams = {0};
    NV0080_ALLOC_PARAMETERS deviceAllocParams = {0};
    NV0080_CTRL_GPU_GET_NUM_SUBDEVICES_PARAMS subdeviceCountParams = {0};
    NV2080_ALLOC_PARAMETERS subdeviceAllocParams = {0};
    RM_API* pRmApi;
    NV_STATUS status;

    if (gpuContext == NULL || clientHandle == NULL ||
        deviceHandle == NULL || subdeviceHandle == NULL || openStage == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *clientHandle = NV01_NULL_OBJECT;
    *deviceHandle = NV01_NULL_OBJECT;
    *subdeviceHandle = NV01_NULL_OBJECT;
    *openStage = NV_RM_GPU_OPEN_STAGE_ROOT_CLIENT;

    securityInfo.privLevel = RS_PRIV_LEVEL_KERNEL;
    securityInfo.paramLocation = PARAM_LOCATION_KERNEL;
    securityInfo.gpuOsInfo = gpuContext;
    securityInfo.clientOSInfo = gpuContext;

    status = gpumgrThreadEnableExpandedGpuVisibility();
    if (status != NV_OK)
    {
        return status;
    }

    pRmApi = rmapiGetInterface(RMAPI_EXTERNAL_KERNEL);
    status = pRmApi->AllocWithSecInfo(
        pRmApi,
        NV01_NULL_OBJECT,
        NV01_NULL_OBJECT,
        clientHandle,
        NV01_ROOT,
        NV_PTR_TO_NvP64(&allocatedClientHandle),
        sizeof(allocatedClientHandle),
        RMAPI_ALLOC_FLAGS_NONE,
        NvP64_NULL,
        &securityInfo);
    if (status != NV_OK)
    {
        goto done;
    }
    if (allocatedClientHandle != *clientHandle)
    {
        status = NV_ERR_INVALID_STATE;
        goto failed;
    }

    *openStage = NV_RM_GPU_OPEN_STAGE_GPU_ID;
    idInfoParams.gpuId = gpuId;
    status = pRmApi->Control(
        pRmApi,
        *clientHandle,
        *clientHandle,
        NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2,
        &idInfoParams,
        sizeof(idInfoParams));
    if (status != NV_OK || idInfoParams.deviceInstance != expectedDeviceInstance)
    {
        status = status == NV_OK ? NV_ERR_INVALID_STATE : status;
        goto failed;
    }

    *openStage = NV_RM_GPU_OPEN_STAGE_DEVICE;
    deviceAllocParams.deviceId = idInfoParams.deviceInstance;
    deviceAllocParams.hClientShare = *clientHandle;
    status = pRmApi->Alloc(
        pRmApi,
        *clientHandle,
        *clientHandle,
        deviceHandle,
        NV01_DEVICE_0,
        &deviceAllocParams,
        sizeof(deviceAllocParams));
    if (status != NV_OK)
    {
        goto failed;
    }

    *openStage = NV_RM_GPU_OPEN_STAGE_SUBDEVICE_COUNT;
    status = pRmApi->Control(
        pRmApi,
        *clientHandle,
        *deviceHandle,
        NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES,
        &subdeviceCountParams,
        sizeof(subdeviceCountParams));
    if (status != NV_OK || subdeviceCountParams.numSubDevices != 1)
    {
        status = status == NV_OK ? NV_ERR_NOT_SUPPORTED : status;
        goto failed;
    }

    *openStage = NV_RM_GPU_OPEN_STAGE_SUBDEVICE;
    subdeviceAllocParams.subDeviceId = 0;
    status = pRmApi->Alloc(
        pRmApi,
        *clientHandle,
        *deviceHandle,
        subdeviceHandle,
        NV20_SUBDEVICE_0,
        &subdeviceAllocParams,
        sizeof(subdeviceAllocParams));
    if (status == NV_OK)
    {
        *openStage = NV_RM_GPU_OPEN_STAGE_COMPLETE;
        goto done;
    }

failed:
    pRmApi->Free(pRmApi, *clientHandle, *clientHandle);
    *clientHandle = NV01_NULL_OBJECT;
    *deviceHandle = NV01_NULL_OBJECT;
    *subdeviceHandle = NV01_NULL_OBJECT;

done:
    gpumgrThreadDisableExpandedGpuVisibility();
    return status;
}

NV_STATUS NvRmCloseGpuDevice(
    NvHandle clientHandle)
{
    RM_API* pRmApi;

    if (clientHandle == NV01_NULL_OBJECT)
    {
        return NV_OK;
    }

    pRmApi = rmapiGetInterface(RMAPI_EXTERNAL_KERNEL);
    return pRmApi->Free(pRmApi, clientHandle, clientHandle);
}

NV_STATUS NvRmCleanupGpuClients(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext)
{
    RM_API* pRmApi;
    NvHandle* pClientHandles = NULL;
    NvU32 clientCount = 0;
    NV_STATUS status;

    if (gpuContext == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (!NvPlatformRmGpuNeedsClientCleanup(gpuContext))
    {
        return NV_OK;
    }

    status = rmapiLockAcquire(RMAPI_LOCK_FLAGS_NONE, RM_LOCK_MODULES_CLIENT);
    if (status != NV_OK)
    {
        return status;
    }

    status = rmapiGetClientHandlesFromOSInfo(
        gpuContext,
        &pClientHandles,
        &clientCount);
    if (status == NV_WARN_NOTHING_TO_DO)
    {
        status = NV_OK;
    }
    else if (status == NV_OK)
    {
        pRmApi = rmapiGetInterface(RMAPI_API_LOCK_INTERNAL);
        status = pRmApi->DisableClients(pRmApi, pClientHandles, clientCount);
        if (status == NV_OK)
        {
            status = serverFreeDisabledClients(
                &g_resServ,
                RM_LOCK_STATES_API_LOCK_ACQUIRED,
                0);
        }
    }

    portMemFree(pClientHandles);

    if (status == NV_OK)
    {
        NvHandle* pRemainingHandles = NULL;
        NvU32 remainingCount = 0;
        NV_STATUS verifyStatus = rmapiGetClientHandlesFromOSInfo(
            gpuContext,
            &pRemainingHandles,
            &remainingCount);

        portMemFree(pRemainingHandles);
        if (verifyStatus != NV_WARN_NOTHING_TO_DO)
        {
            status = verifyStatus == NV_OK ? NV_ERR_IN_USE : verifyStatus;
        }
    }

    rmapiLockRelease();

    if (status == NV_OK)
    {
        NvPlatformRmGpuCompleteClientCleanup(gpuContext);
    }

    return status;
}