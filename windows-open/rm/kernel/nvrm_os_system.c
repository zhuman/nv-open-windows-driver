#include "g_system_nvoc.h"
#include "g_os_nvoc.h"
#include "nvplatform_registry_rm.h"
#include "nvplatform_system_rm.h"

static const char BugCheckOnTimeoutValueName[] = "BugCheckOnTimeout";

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

void osSyncWithRmDestroy(void)
{
    NvPlatformRmDrainSystemWork();
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

NvBool hypervisorIsVgxHyper_IMPL(void)
{
    return NV_FALSE;
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

void osBugCheck(NvU32 bugCode)
{
    NvPlatformRmBugCheck(
        bugCode <= OS_BUG_CHECK_BUGCODE_LAST
            ? bugCode
            : OS_BUG_CHECK_BUGCODE_UNKNOWN);
}