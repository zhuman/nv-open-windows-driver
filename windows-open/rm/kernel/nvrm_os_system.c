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

void osAssertFailed(void)
{
    NvPlatformRmCaptureAssertion();
}

void osBugCheck(NvU32 bugCode)
{
    NvPlatformRmBugCheck(
        bugCode <= OS_BUG_CHECK_BUGCODE_LAST
            ? bugCode
            : OS_BUG_CHECK_BUGCODE_UNKNOWN);
}