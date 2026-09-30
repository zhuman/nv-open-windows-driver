#include "core/core.h"
#include "core/locks.h"
#include "gpu/bus/kern_bus.h"
#include "gpu/disp/inst_mem/disp_inst_mem.h"
#include "gpu/disp/kern_disp.h"
#include "gpu/fifo/kernel_fifo.h"
#include "gpu/gr/kernel_graphics.h"
#include "kernel/gpu/mig_mgr/kernel_mig_manager.h"
#include "gpu/gpu.h"
#include "gpu/gsp/kernel_gsp.h"
#include "kernel/mem_mgr/mem.h"
#include "gpu_mgr/gpu_mgr.h"
#include "nvplatform_mmio_rm.h"
#include "rmapi/alloc_size.h"
#include "rmapi/rmapi.h"
#include "g_rs_server_nvoc.h"
#include "nvrm_gpu_attach.h"

#include "published/turing/tu102/dev_ext_devices.h"
#include "published/turing/tu102/dev_vm.h"

#include <class/cl0000.h>
#include <class/cl0080.h>
#include <class/cl2080.h>
#include <class/clc361.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl0080/ctrl0080gpu.h>
#include <ctrl/ctrl0080/ctrl0080fb.h>
#include <ctrl/ctrl2080/ctrl2080bus.h>
#include <ctrl/ctrl2080/ctrl2080unix.h>

#define NV_RM_PROM_SIZE 0x100000U

void Nv04AllocWithAccessKernel(NVOS64_PARAMETERS* params);
void Nv01AllocMemoryWithSecInfo(NVOS02_PARAMETERS* params, API_SECURITY_INFO securityInfo);
void Nv04ControlKernel(NVOS54_PARAMETERS* params);
void NvKmsWindowsTraceMessage(const char* message);

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

NV_STATUS NvRmInitializeGpuFirmware(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    const void* firmware,
    NvU32 firmwareSize)
{
    GSP_FIRMWARE gspFirmware = {0};
    OBJGPU* pGpu = NULL;
    DEVICE_MAPPING* pGpuMapping = NULL;
    volatile void* pPromAddress = NULL;
    void* publishedGpu = NULL;
    NvU32 previousGpuInstance = NV_U32_MAX;
    GPU_MASK gpusLockedMask = 0;
    NV_STATUS status;

    if (gpuContext == NULL || firmware == NULL || firmwareSize == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
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

    status = NvPlatformRmGpuGetPublishedRmGpu(gpuContext, &publishedGpu);
    if (status == NV_OK)
    {
        pGpu = (OBJGPU*)publishedGpu;
        previousGpuInstance = gpumgrSetCurrentGpuInstance(pGpu->gpuInstance);
        pGpuMapping = gpuGetDeviceMapping(pGpu, DEVICE_INDEX_GPU, 0);
        if (pGpuMapping == NULL || pGpuMapping->gpuNvAddr == NULL)
        {
            status = NV_ERR_INVALID_STATE;
        }
        else
        {
            pPromAddress = (volatile NvU8*)pGpuMapping->gpuNvAddr + NV_PROM_DATA(0);
            status = NvPlatformRmBeginRegisterCache32(
                pPromAddress, NV_RM_PROM_SIZE / sizeof(NvU32));
        }
        if (status == NV_OK)
        {
            gspFirmware.pBuf = firmware;
            gspFirmware.size = firmwareSize;
            NvKmsWindowsTraceMessage("RM GPU lifecycle: GSP init begin");
            status = kgspInitRm(pGpu, GPU_GET_KERNEL_GSP(pGpu), &gspFirmware);
            NvKmsWindowsTraceMessage("RM GPU lifecycle: GSP init end");
            NvPlatformRmEndRegisterCache32(pPromAddress);
        }
        if (status == NV_OK)
        {
            NvKmsWindowsTraceMessage("RM GPU lifecycle: pre-init begin");
            status = gpumgrStatePreInitGpu(pGpu);
            NvKmsWindowsTraceMessage("RM GPU lifecycle: pre-init end");
        }
        if (status == NV_OK)
        {
            NvKmsWindowsTraceMessage("RM GPU lifecycle: state init begin");
            status = gpumgrStateInitGpu(pGpu);
            NvKmsWindowsTraceMessage("RM GPU lifecycle: state init end");
        }
        if (status == NV_OK)
        {
            status = rmGpuGroupLockAcquire(
                pGpu->gpuInstance,
                GPU_LOCK_GRP_SUBDEVICE,
                GPUS_LOCK_FLAGS_NONE,
                RM_LOCK_MODULES_INIT,
                &gpusLockedMask);
        }
        if (status == NV_OK)
        {
            KernelGraphics* pKernelGraphics = GPU_GET_KERNEL_GRAPHICS(pGpu, 0);

            NvKmsWindowsTraceMessage(
                rmDeviceGpuLockIsOwner(pGpu->gpuInstance)
                    ? "RM GPU lifecycle: graphics GPU lock owned"
                    : "RM GPU lifecycle: graphics GPU lock not owned");
            NvKmsWindowsTraceMessage("RM GPU lifecycle: graphics static info begin");
            status = pKernelGraphics != NULL
                ? kgraphicsLoadStaticInfo(
                    pGpu,
                    pKernelGraphics,
                    KMIGMGR_SWIZZID_INVALID)
                : NV_ERR_INVALID_STATE;
            NvKmsWindowsTraceMessage("RM GPU lifecycle: graphics static info end");
        }
        if (status == NV_OK)
        {
            KernelDisplay* pKernelDisplay = GPU_GET_KERNEL_DISPLAY(pGpu);

            NvKmsWindowsTraceMessage("RM GPU lifecycle: display instmem load begin");
            status = instmemStateLoad(
                pGpu,
                KERNEL_DISPLAY_GET_INST_MEM(pKernelDisplay),
                0);
            NvKmsWindowsTraceMessage("RM GPU lifecycle: display instmem load end");
        }
        if (gpusLockedMask != 0)
        {
            rmGpuGroupLockRelease(gpusLockedMask, GPUS_LOCK_FLAGS_NONE);
        }
        gpumgrSetCurrentGpuInstance(previousGpuInstance);
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

static API_SECURITY_INFO NvRmGetExternalKernelSecurityInfo(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext)
{
    API_SECURITY_INFO securityInfo = {0};
    securityInfo.privLevel = RS_PRIV_LEVEL_KERNEL;
    securityInfo.paramLocation = PARAM_LOCATION_KERNEL;
    securityInfo.gpuOsInfo = gpuContext;
    securityInfo.clientOSInfo = gpuContext;
    return securityInfo;
}

void NvRmExternalKernelAlloc(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* paramsPointer)
{
    NVOS64_PARAMETERS* params = paramsPointer;
    RM_API* pRmApi;
    API_SECURITY_INFO securityInfo;
    NvBool allowNull;
    NvU32 paramsSize;
    NV_STATUS status;

    if (gpuContext == NULL || params == NULL)
    {
        if (params != NULL)
        {
            params->status = NV_ERR_INVALID_ARGUMENT;
        }
        return;
    }

    securityInfo = NvRmGetExternalKernelSecurityInfo(gpuContext);
    pRmApi = rmapiGetInterface(RMAPI_EXTERNAL_KERNEL);
    status = gpumgrThreadEnableExpandedGpuVisibility();
    if (status == NV_OK)
    {
        if (params->hClass == VOLTA_USERMODE_A)
        {
            void* publishedGpu = NULL;
            status = NvPlatformRmGpuGetPublishedRmGpu(gpuContext, &publishedGpu);
            if (status == NV_OK)
            {
                OBJGPU* pGpu = publishedGpu;
                KernelFifo* pKernelFifo = GPU_GET_KERNEL_FIFO(pGpu);
                NvU32 attr = 0;
                NvU32 attr2 = 0;
                NvU64 offset;
                NvU32 previousGpuInstance =
                    gpumgrSetCurrentGpuInstance(pGpu->gpuInstance);

                if (pKernelFifo == NULL)
                {
                    status = NV_ERR_INVALID_STATE;
                }
                else if (pKernelFifo->pRegVF == NULL)
                {
                    attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY,
                                       _CONTIGUOUS, attr);
                    attr = FLD_SET_DRF(OS32, _ATTR, _COHERENCY,
                                       _CACHED, attr);
                    attr2 = FLD_SET_DRF(OS32, _ATTR2, _GPU_CACHEABLE,
                                        _NO, attr2);
                    offset = GPU_GET_VREG_OFFSET(
                        pGpu,
                        DRF_BASE(NV_VIRTUAL_FUNCTION));

                    status = memCreateMemDesc(
                        pGpu,
                        &pKernelFifo->pRegVF,
                        ADDR_REGMEM,
                        offset,
                        DRF_SIZE(NVC361),
                        attr,
                        attr2);
                    if (status == NV_OK)
                    {
                        memdescSetFlag(
                            pKernelFifo->pRegVF,
                            MEMDESC_FLAGS_SKIP_REGMEM_PRIV_CHECK,
                            NV_TRUE);
                    }
                }

                gpumgrSetCurrentGpuInstance(previousGpuInstance);
            }
        }

        paramsSize = params->paramsSize;
        if (status == NV_OK && paramsSize == 0 && params->hClass != NV01_ROOT)
        {
            status = rmapiGetClassAllocParamSize(
                &paramsSize,
                params->pAllocParms,
                &allowNull,
                params->hClass);
        }
        if (status == NV_OK)
        {
            params->status = pRmApi->AllocWithSecInfo(
                pRmApi,
                params->hRoot,
                params->hObjectParent,
                &params->hObjectNew,
                params->hClass,
                params->pAllocParms,
                paramsSize,
                RMAPI_ALLOC_FLAGS_NONE,
                params->pRightsRequested,
                &securityInfo);
        }
        else
        {
            params->status = status;
        }
        gpumgrThreadDisableExpandedGpuVisibility();
    }
    else
    {
        params->status = status;
    }
}

void NvRmExternalKernelControl(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* paramsPointer)
{
    NVOS54_PARAMETERS* params = paramsPointer;
    NV_STATUS status;

    if (gpuContext == NULL || params == NULL)
    {
        if (params != NULL)
        {
            params->status = NV_ERR_INVALID_ARGUMENT;
        }
        return;
    }

    status = gpumgrThreadEnableExpandedGpuVisibility();
    if (status == NV_OK)
    {
        if (params->cmd == NV2080_CTRL_CMD_BUS_GET_INFO)
        {
            NV2080_CTRL_BUS_GET_INFO_PARAMS* busParams =
                NvP64_VALUE(params->params);
            NV2080_CTRL_BUS_INFO* busInfoList =
                NvP64_VALUE(busParams->busInfoList);
            NvU32 i;

            for (i = 0; i < busParams->busInfoListSize; i++)
            {
                if (busInfoList[i].index ==
                    NV2080_CTRL_BUS_INFO_INDEX_COHERENT_DMA_FLAGS)
                {
                    busInfoList[i].data = DRF_DEF(
                        2080_CTRL_BUS_INFO,
                        _COHERENT_DMA_FLAGS,
                        _CTXDMA,
                        _TRUE);
                }
                else if (busInfoList[i].index ==
                         NV2080_CTRL_BUS_INFO_INDEX_NONCOHERENT_DMA_FLAGS)
                {
                    busInfoList[i].data = 0;
                }
            }
            params->status = NV_OK;
        }
        else if (params->cmd == NV0080_CTRL_CMD_FB_GET_CAPS_V2)
        {
            NV0080_CTRL_FB_GET_CAPS_V2_PARAMS* capsV2 =
                NvP64_VALUE(params->params);
            portMemSet(capsV2->capsTbl, 0, sizeof(capsV2->capsTbl));
            capsV2->capsTbl[1] = 0x80;
            params->status = NV_OK;
        }
        else if (params->cmd == NV2080_CTRL_CMD_GPU_GET_ENGINES_V2)
        {
            NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS* engines =
                NvP64_VALUE(params->params);
            engines->engineCount = 1;
            engines->engineList[0] = NV2080_ENGINE_TYPE_GRAPHICS;
            params->status = NV_OK;
        }
        else if (params->cmd == NV2080_CTRL_CMD_OS_UNIX_GC6_BLOCKER_REFCNT)
        {
            params->status = NV_OK;
        }
        else
        {
            Nv04ControlKernel(params);
        }
        gpumgrThreadDisableExpandedGpuVisibility();
    }
    else
    {
        params->status = status;
    }
}

void NvRmExternalKernelAllocMemory(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* paramsPointer)
{
    NVOS02_PARAMETERS* params = paramsPointer;
    API_SECURITY_INFO securityInfo;
    NV_STATUS status;

    if (gpuContext == NULL || params == NULL)
    {
        if (params != NULL)
        {
            params->status = NV_ERR_INVALID_ARGUMENT;
        }
        return;
    }

    securityInfo = NvRmGetExternalKernelSecurityInfo(gpuContext);
    status = gpumgrThreadEnableExpandedGpuVisibility();
    if (status == NV_OK)
    {
        Nv01AllocMemoryWithSecInfo(params, securityInfo);
        gpumgrThreadDisableExpandedGpuVisibility();
    }
    else
    {
        params->status = status;
    }
}

void NvRmExternalKernelFree(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* paramsPointer)
{
    NVOS00_PARAMETERS* params = paramsPointer;
    RM_API* pRmApi;

    if (gpuContext == NULL || params == NULL)
    {
        if (params != NULL)
        {
            params->status = NV_ERR_INVALID_ARGUMENT;
        }
        return;
    }

    pRmApi = rmapiGetInterface(RMAPI_EXTERNAL_KERNEL);
    params->status = pRmApi->Free(
        pRmApi,
        params->hRoot,
        params->hObjectOld);
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