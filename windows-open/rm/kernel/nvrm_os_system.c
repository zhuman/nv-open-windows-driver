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
