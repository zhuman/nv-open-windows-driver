#include "gpu/gpu.h"
#include "gpu/timer/objtmr.h"
#include "gpu_mgr/gpu_mgr.h"
#include "core/thread_state.h"
#include "nvplatform_gpu_rm.h"
#include "nvplatform_time_rm.h"

static void serviceNanoTimerWorkItem(NvU32 gpuInstance, void* timerEvent)
{
    OBJGPU* pGpu = gpumgrGetGpu(gpuInstance);
    if (pGpu != NULL)
    {
        OBJTMR* pTmr = GPU_GET_TIMER(pGpu);
        (void)tmrEventServiceTimer(pGpu, pTmr, (TMR_EVENT*)timerEvent);
    }
}

static void queueNanoTimerWorkItem(void* gpu, void* timerEvent)
{
    OBJGPU* pGpu = (OBJGPU*)gpu;
    THREAD_STATE_NODE threadState;

    if (!FULL_GPU_SANITY_FOR_PM_RESUME(pGpu))
    {
        return;
    }

    threadStateInitISRAndDeferredIntHandler(
        &threadState,
        pGpu,
        THREAD_STATE_FLAGS_IS_DEFERRED_INT_HANDLER);
    (void)osQueueWorkItem(
        pGpu,
        serviceNanoTimerWorkItem,
        timerEvent,
        (OsQueueWorkItemFlags){
            .bDontFreeParams = NV_TRUE,
            .bLockGpuGroupDevice = NV_TRUE});
    threadStateFreeISRAndDeferredIntHandler(
        &threadState,
        pGpu,
        THREAD_STATE_FLAGS_IS_DEFERRED_INT_HANDLER);
}

NV_STATUS osGetSystemTime(NvU32* seconds, NvU32* microseconds)
{
    return NvPlatformGetRmSystemTime(seconds, microseconds);
}

NvU64 osGetTimestamp(void)
{
    return NvPlatformGetRmTimestamp();
}

NvU64 osGetTimestampFreq(void)
{
    return NvPlatformGetRmTimestampFrequency();
}

NvU64 osGetMonotonicTimeNs(void)
{
    return NvPlatformGetRmMonotonicTimeNs();
}

NV_STATUS osGetPerformanceCounter(NvU64* timeNs)
{
    return NvPlatformGetRmPerformanceCounter(timeNs);
}

NV_STATUS osDelay(NvU32 milliseconds)
{
    return NvPlatformDelayRmMilliseconds(milliseconds);
}

NV_STATUS osDelayUs(NvU32 microseconds)
{
    return NvPlatformDelayRmMicroseconds(microseconds);
}

NV_STATUS osDelayNs(NvU32 nanoseconds)
{
    NvU32 microseconds = nanoseconds / 1000;
    if (microseconds == 0)
    {
        microseconds = 1;
    }

    return NvPlatformDelayRmMicroseconds(microseconds);
}

NV_STATUS osCreateNanoTimer(
    OS_GPU_INFO* pOsGpuInfo,
    void* timerEvent,
    void** timer)
{
    void* pGpu = NULL;
    NV_STATUS status = NvPlatformRmGpuGetPublishedRmGpu(
        (NV_PLATFORM_RM_GPU_CONTEXT*)pOsGpuInfo,
        &pGpu);
    if (status != NV_OK)
    {
        return status;
    }

    return NvPlatformRmNanoTimerCreate(
        queueNanoTimerWorkItem,
        pGpu,
        timerEvent,
        timer);
}

NV_STATUS osStartNanoTimer(
    OS_GPU_INFO* pOsGpuInfo,
    void* timer,
    NvU64 timeNs)
{
    (void)pOsGpuInfo;
    return NvPlatformRmNanoTimerStart(timer, timeNs);
}

NV_STATUS osCancelNanoTimer(OS_GPU_INFO* pOsGpuInfo, void* timer)
{
    (void)pOsGpuInfo;
    return NvPlatformRmNanoTimerCancel(timer);
}

NV_STATUS osDestroyNanoTimer(OS_GPU_INFO* pOsGpuInfo, void* timer)
{
    (void)pOsGpuInfo;
    return NvPlatformRmNanoTimerDestroy(timer);
}