#include "g_system_nvoc.h"
#include "g_os_nvoc.h"
#include "g_gpu_nvoc.h"
#include "g_gpu_mgr_nvoc.h"
#include "kernel/core/locks.h"
#include "lib/protobuf/prb.h"
#include "g_nvdebug_pb.h"
#include "nvplatform_file_rm.h"
#include "nvplatform_registry_rm.h"
#include "nvplatform_system_rm.h"

static const char BugCheckOnTimeoutValueName[] = "BugCheckOnTimeout";

typedef struct
{
    OSWorkItemFunction* function;
    void* parameters;
    NvU32 gpuInstance;
    OsQueueWorkItemFlags flags;
} WindowsRmWorkItem;

static void executeRmWorkItem(void* context)
{
    WindowsRmWorkItem* workItem = (WindowsRmWorkItem*)context;
    OsQueueWorkItemFlags releaseLocks = {0};
    NvU32 gpuMask = 0;

    if (workItemLocksAcquire(
            workItem->gpuInstance,
            workItem->flags,
            &releaseLocks,
            &gpuMask) == NV_OK)
    {
        // Enter rundown only after acquiring RM locks. GPU destruction holds
        // the API lock while draining this gate, so the reverse order would
        // deadlock queued work that requested that lock.
        if (NvPlatformRmGpuWorkAcquire())
        {
            OBJGPU* pGpu = gpumgrGetGpu(workItem->gpuInstance);
            if (pGpu != NULL &&
                !(workItem->flags.bDropOnUnloadQueueFlush &&
                  !NvPlatformRmSystemWorkIsActive()))
            {
                NvU32 previousGpuInstance =
                    gpumgrSetCurrentGpuInstance(workItem->gpuInstance);
                workItem->function(workItem->gpuInstance, workItem->parameters);
                gpumgrSetCurrentGpuInstance(previousGpuInstance);
            }
            NvPlatformRmGpuWorkRelease();
        }
    }

    workItemLocksRelease(releaseLocks, gpuMask);
    if (workItem->parameters != NULL && !workItem->flags.bDontFreeParams)
    {
        portMemFree(workItem->parameters);
    }
    portMemFree(workItem);
}

void osInitSystemStaticConfig(SYS_STATIC_CONFIG* config)
{
    if (config == NULL)
    {
        return;
    }

    config->bIsNotebook = NvPlatformRmIsNotebook();
}

NvBool osBugCheckOnTimeoutEnabled(void)
{
    NvU32 enabled = 0;
    return NvPlatformReadRmRegistryDword(
        BugCheckOnTimeoutValueName,
        &enabled) == NV_OK && enabled != 0
        ? NV_TRUE
        : NV_FALSE;
}

NV_STATUS osQueueSystemWorkItem(
    void (*function)(void*),
    void* context)
{
    return NvPlatformRmQueueSystemWork(function, context);
}

NV_STATUS osQueueWorkItem(
    OBJGPU* pGpu,
    OSWorkItemFunction function,
    void* parameters,
    OsQueueWorkItemFlags flags)
{
    WindowsRmWorkItem* workItem;
    NV_STATUS status;

    if (pGpu == NULL || function == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    workItem = portMemAllocNonPaged(sizeof(*workItem));
    if (workItem == NULL)
    {
        return NV_ERR_NO_MEMORY;
    }

    workItem->function = function;
    workItem->parameters = parameters;
    workItem->gpuInstance = gpuGetInstance(pGpu);
    workItem->flags = flags;
    workItem->flags.bRequiresGpu = NV_TRUE;

    status = NvPlatformRmQueueSystemWork(executeRmWorkItem, workItem);
    if (status != NV_OK)
    {
        portMemFree(workItem);
    }
    return status;
}

void osSyncWithRmDestroy(void)
{
    NvPlatformRmDrainSystemWork();
}

void osSyncWithGpuDestroy(NvBool bEntry)
{
    if (bEntry)
    {
        NvPlatformRmGpuDestroyBegin();
    }
    else
    {
        NvPlatformRmGpuDestroyEnd();
    }
}

NV_STATUS osAcquireRmSema(void* pSema)
{
    (void)pSema;
    return NvPlatformRmSemaAcquire();
}

NV_STATUS osCondAcquireRmSema(void* pSema)
{
    (void)pSema;
    return NvPlatformRmSemaTryAcquire();
}

NvU32 osReleaseRmSema(void* pSema, OBJGPU* pDpcGpu)
{
    (void)pSema;
    (void)pDpcGpu;
    NvPlatformRmSemaRelease();
    return NV_SEMA_RELEASE_SUCCEED;
}

NvBool osIsAdministrator(void)
{
    return NvPlatformRmIsCurrentSubjectAdministrator();
}

NvBool osDmabufIsSupported(void)
{
    return NV_FALSE;
}

NvBool osImexChannelIsSupported(void)
{
    return NV_FALSE;
}

NvS32 osImexChannelCount(void)
{
    return 0;
}

NvS32 osImexChannelGet(NvU64 descriptor)
{
    (void)descriptor;
    return -1;
}

NV_STATUS osQueueDrainP2PHandler(NvU8* pUuid)
{
    (void)pUuid;
    return NV_ERR_NOT_SUPPORTED;
}

void osQueueResumeP2PHandler(NvU8* pUuid)
{
    (void)pUuid;
}

void osRmCapInitDescriptor(NvU64* capDescriptor)
{
    if (capDescriptor != NULL)
    {
        *capDescriptor = NV_U64_MAX;
    }
}

NV_STATUS osRmCapAcquire(
    OS_RM_CAPS* osRmCaps,
    NvU32 rmCap,
    NvU64 capDescriptor,
    NvU64* duplicatedCapDescriptor)
{
    (void)osRmCaps;
    (void)rmCap;
    (void)capDescriptor;
    osRmCapInitDescriptor(duplicatedCapDescriptor);
    return NV_ERR_NOT_SUPPORTED;
}

void osRmCapRelease(NvU64 duplicatedCapDescriptor)
{
    (void)duplicatedCapDescriptor;
}

NV_STATUS osRmCapRegisterSys(OS_RM_CAPS** osRmCaps)
{
    if (osRmCaps != NULL)
    {
        *osRmCaps = NULL;
    }
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osRmCapRegisterGpu(
    OS_GPU_INFO* osGpuInfo,
    OS_RM_CAPS** osRmCaps)
{
    (void)osGpuInfo;
    if (osRmCaps != NULL)
    {
        *osRmCaps = NULL;
    }
    return NV_OK;
}

NV_STATUS osRmCapRegisterSmcPartition(
    OS_RM_CAPS* gpuOsRmCaps,
    OS_RM_CAPS** partitionOsRmCaps,
    NvU32 partitionId)
{
    (void)gpuOsRmCaps;
    (void)partitionId;
    if (partitionOsRmCaps != NULL)
    {
        *partitionOsRmCaps = NULL;
    }
    return NV_OK;
}

NV_STATUS osRmCapRegisterSmcExecutionPartition(
    OS_RM_CAPS* partitionOsRmCaps,
    OS_RM_CAPS** executionPartitionOsRmCaps,
    NvU32 executionPartitionId)
{
    (void)partitionOsRmCaps;
    (void)executionPartitionId;
    if (executionPartitionOsRmCaps != NULL)
    {
        *executionPartitionOsRmCaps = NULL;
    }
    return NV_OK;
}

void osRmCapUnregister(OS_RM_CAPS** osRmCaps)
{
    if (osRmCaps != NULL)
    {
        *osRmCaps = NULL;
    }
}

NvCgroupImpl osCgroupImplementation(void)
{
    return CGROUP_IMPL_NONE;
}

ClientGroupID osClientGroupID(NvU64 process, void* pidInfo)
{
    (void)process;
    (void)pidInfo;
    return 0;
}

void* osCgroupRegisterRegion(OBJGPU* pGpu, NvU64 size)
{
    (void)pGpu;
    (void)size;
    return NULL;
}

void osCgroupUnregisterRegion(void* region)
{
    (void)region;
}

NV_STATUS osMemacctTryCharge(
    void* region,
    NvLength size,
    void** pool)
{
    (void)region;
    (void)size;
    if (pool != NULL)
    {
        *pool = NULL;
    }
    return NV_ERR_NOT_SUPPORTED;
}

void osMemacctReleaseCharge(void* pool, NvLength size)
{
    (void)pool;
    (void)size;
}

NV_STATUS osTegraSocGetImpImportData(
    OBJGPU* pGpu,
    TEGRA_IMP_IMPORT_DATA* importData)
{
    (void)pGpu;
    (void)importData;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osTegraSocGetImpUefiData(
    OS_GPU_INFO* osGpuInfo,
    NvU32* isoBandwidthKbps,
    NvU32* floorBandwidthKbps)
{
    (void)osGpuInfo;
    (void)isoBandwidthKbps;
    (void)floorBandwidthKbps;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osTegraAllocateDisplayBandwidth(
    OS_GPU_INFO* osGpuInfo,
    NvU32 averageBandwidthKbps,
    NvU32 floorBandwidthKbps)
{
    (void)osGpuInfo;
    (void)averageBandwidthKbps;
    (void)floorBandwidthKbps;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osTegraiGpuPerfBoost(
    OBJGPU* pGpu,
    NvBool enable,
    NvU32 duration)
{
    (void)pGpu;
    (void)enable;
    (void)duration;
    return NV_ERR_NOT_SUPPORTED;
}

NvBool osGpuIsCxlDevice(OBJGPU* pGpu)
{
    (void)pGpu;
    return NV_FALSE;
}

void osCxlSetCaching(OBJGPU* pGpu, NvBool enableCache)
{
    (void)pGpu;
    (void)enableCache;
}

NvBool osIsGridSupported(OBJGPU* pGpu)
{
    (void)pGpu;
    return NV_FALSE;
}

NvU32 osGetGridCspSupport(void)
{
    return 0;
}

NvBool osIsNvswitchPresent(void)
{
    return NV_FALSE;
}

NV_STATUS osIsVgpuDeviceVmPresent(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvBool osRemoveGpuSupported(void)
{
    return NV_FALSE;
}

void osRemoveGpu(NvU32 domain, NvU8 bus, NvU8 device)
{
    (void)domain;
    (void)bus;
    (void)device;
}

NvBool osIsSwPreInitOnly(OS_GPU_INFO* pOsGpuInfo)
{
    (void)pOsGpuInfo;
    return NV_FALSE;
}

NvBool osGpuSupportsAts(OBJGPU* pGpu)
{
    (void)pGpu;
    return NV_FALSE;
}

NvU32 osGetDynamicPowerSupportMask(void)
{
    return 0;
}

void osClientGcoffDisallowRefcount(
    OS_GPU_INFO* pOsGpuInfo,
    NvBool disallow)
{
    (void)pOsGpuInfo;
    (void)disallow;
}

NV_STATUS osRefGpuAccessNeeded(OS_GPU_INFO* osGpuInfo)
{
    (void)osGpuInfo;
    return NV_OK;
}

void osUnrefGpuAccessNeeded(OS_GPU_INFO* osGpuInfo)
{
    (void)osGpuInfo;
}

NV_STATUS osGetSyncpointAperture(
    OS_GPU_INFO* osGpuInfo,
    NvU32 syncpointId,
    NvU64* physicalAddress,
    NvU64* limit,
    NvU32* offset)
{
    (void)osGpuInfo;
    (void)syncpointId;
    (void)physicalAddress;
    (void)limit;
    (void)offset;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osSimEscapeWrite(
    OBJGPU* pGpu,
    const char* path,
    NvU32 index,
    NvU32 size,
    NvU32 value)
{
    (void)pGpu;
    (void)path;
    (void)index;
    (void)size;
    (void)value;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osSimEscapeWriteBuffer(
    OBJGPU* pGpu,
    const char* path,
    NvU32 index,
    NvU32 size,
    void* buffer)
{
    (void)pGpu;
    (void)path;
    (void)index;
    (void)size;
    (void)buffer;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osSimEscapeRead(
    OBJGPU* pGpu,
    const char* path,
    NvU32 index,
    NvU32 size,
    NvU32* value)
{
    (void)pGpu;
    (void)path;
    (void)index;
    (void)size;
    (void)value;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osSimEscapeReadBuffer(
    OBJGPU* pGpu,
    const char* path,
    NvU32 index,
    NvU32 size,
    void* buffer)
{
    (void)pGpu;
    (void)path;
    (void)index;
    (void)size;
    (void)buffer;
    return NV_ERR_NOT_SUPPORTED;
}

NvBool hypervisorIsVgxHyper_IMPL(void)
{
    return NV_FALSE;
}

NV_STATUS osIsVgpuVfioPresent(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osIsVfioPciCorePresent(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

void osWakeRemoveVgpu(NvU32 gpuId, NvU32 returnStatus)
{
    (void)gpuId;
    (void)returnStatus;
}

void osModifyGpuSwStatePersistence(OS_GPU_INFO* osGpuInfo, NvBool enable)
{
    (void)osGpuInfo;
    (void)enable;
}

NV_STATUS osGC6PowerControl(
    OBJGPU* pGpu,
    NvU32 function,
    NvU32* result)
{
    (void)pGpu;
    (void)function;
    (void)result;
    return NV_ERR_NOT_SUPPORTED;
}

void osDisableConsoleManagement(OBJGPU* pGpu)
{
    (void)pGpu;
}

NV_STATUS hypervisorInjectInterrupt_IMPL(
    void* pHypervisor,
    void* pVgpuNsIntr)
{
    (void)pHypervisor;
    (void)pVgpuNsIntr;
    return NV_ERR_NOT_SUPPORTED;
}

void osAssertFailed(void)
{
    NvPlatformRmCaptureAssertion();
}

void os_dump_stack(void)
{
    NvPlatformRmCaptureAssertion();
}

NV_STATUS os_iommu_sva_bind(void* arg, void** handle, NvU32* pasid)
{
    (void)arg;

    if (handle != NULL)
    {
        *handle = NULL;
    }
    if (pasid != NULL)
    {
        *pasid = 0;
    }

    return NV_ERR_NOT_SUPPORTED;
}

void os_iommu_sva_unbind(void* handle)
{
    (void)handle;
}

NvU32 osApiLockAcquireConfigureFlags(NvU32 flags)
{
    return flags;
}

NV_STATUS osGpuLocksQueueRelease(
    OBJGPU* gpu,
    NvU32 releaseMode)
{
    (void)gpu;
    (void)releaseMode;
    return NV_SEMA_RELEASE_FAILED;
}

NV_STATUS osGetUefiVariable(
    const char* name,
    LPGUID vendorGuid,
    NvU8* data,
    NvU32* dataLength)
{
    return NvPlatformRmGetFirmwareVariable(
        name,
        vendorGuid,
        data,
        dataLength);
}

NV_STATUS osInitGetAcpiTable(void)
{
    return NV_OK;
}

NV_STATUS osGetSmbiosTable(
    void** ppBaseVAddr,
    NvU64* pLength,
    NvU64* pNumSubTypes,
    NvU32* pVersion)
{
    return NvPlatformRmGetSmbiosTable(
        ppBaseVAddr,
        pLength,
        pNumSubTypes,
        pVersion);
}

void osPutSmbiosTable(void* pBaseVAddr, NvU64 length)
{
    (void)length;
    NvPlatformRmPutSmbiosTable(pBaseVAddr);
}

NV_STATUS osGetAcpiTable(
    NvU32 tableSignature,
    void** ppTable,
    NvU32 tableSize,
    NvU32* retSize)
{
    return NvPlatformRmGetAcpiTable(
        tableSignature,
        ppTable != NULL ? *ppTable : NULL,
        tableSize,
        retSize);
}

void osBugCheck(NvU32 bugCode)
{
    NvPlatformRmBugCheck(
        bugCode <= OS_BUG_CHECK_BUGCODE_LAST
            ? bugCode
            : OS_BUG_CHECK_BUGCODE_UNKNOWN);
}

NvBool osDbgBreakpointEnabled(void)
{
    return NvPlatformRmIsDebuggerPresent();
}

void osDbgBugCheckOnAssert(void)
{
    NvPlatformRmBugCheck(OS_BUG_CHECK_BUGCODE_BSOD_ON_ASSERT);
}

NV_STATUS osOpenTemporaryFile(void** ppFile)
{
    return NvPlatformRmOpenTemporaryFile(ppFile);
}

void osCloseFile(void* pFile)
{
    NvPlatformRmCloseFile(pFile);
}

NV_STATUS osWriteToFile(
    void* pFile,
    NvU8* buffer,
    NvU64 size,
    NvU64 offset)
{
    return NvPlatformRmWriteFile(pFile, buffer, size, offset);
}

NV_STATUS osReadFromFile(
    void* pFile,
    NvU8* buffer,
    NvU64 size,
    NvU64 offset)
{
    return NvPlatformRmReadFile(pFile, buffer, size, offset);
}

NV_STATUS osGetDriverBlock(
    OS_GPU_INFO* pOsGpuInfo,
    OS_DRIVER_BLOCK* pBlock)
{
    (void)pOsGpuInfo;
    if (pBlock == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    return NvPlatformRmGetDriverImageInfo(
        &pBlock->driverStart,
        pBlock->unique_id,
        &pBlock->age,
        &pBlock->offset);
}

NV_STATUS osGetVersionDump(void* pEncoder)
{
    NvU32 majorVersion;
    NvU32 minorVersion;
    NvU32 buildNumber;
    NvU32 productType;
    NV_STATUS status;

    if (pEncoder == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = NvPlatformRmGetOsVersion(
        &majorVersion,
        &minorVersion,
        &buildNumber,
        &productType);
    if (status != NV_OK)
    {
        return status;
    }

    prbEncAddUInt32(
        (PRB_ENCODER*)pEncoder,
        NVDEBUG_SYSTEMINFO_OSINFO_FAMILY,
        NVDEBUG_OS_NT);
    prbEncAddUInt32(
        (PRB_ENCODER*)pEncoder,
        NVDEBUG_SYSTEMINFO_OSINFO_OSMAJORVERSION,
        majorVersion);
    prbEncAddUInt32(
        (PRB_ENCODER*)pEncoder,
        NVDEBUG_SYSTEMINFO_OSINFO_OSMINORVERSION,
        minorVersion);
    prbEncAddUInt32(
        (PRB_ENCODER*)pEncoder,
        NVDEBUG_SYSTEMINFO_OSINFO_OSBLDNUM,
        buildNumber);
    prbEncAddUInt32(
        (PRB_ENCODER*)pEncoder,
        NVDEBUG_SYSTEMINFO_OSINFO_WINPRODUCTTYPE,
        productType);
    return NV_OK;
}

void osAddRecordForCrashLog(void* pBuffer, NvU32 length)
{
    (void)NvPlatformRmAddCrashLog(pBuffer, length);
}

void osDeleteRecordForCrashLog(void* pBuffer)
{
    NvPlatformRmDeleteCrashLog(pBuffer);
}

void osErrorLogV(
    OBJGPU* pGpu,
    XidContext context,
    const char* pFormat,
    va_list arglist)
{
    (void)pGpu;
    NvPlatformRmLogErrorV(context.xid, pFormat, arglist);
}
