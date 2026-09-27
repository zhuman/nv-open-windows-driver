#include "nvplatform_system_rm.h"

#include <windows.h>

namespace {

constexpr LONG SystemWorkStateNeverInitialized = 0;
constexpr LONG SystemWorkStateActive = 1;
constexpr LONG SystemWorkStateStopped = 2;

struct SystemWorkItem
{
    NvPlatformRmSystemWorkFunction* function;
    void* context;
};

SRWLOCK systemWorkLock = SRWLOCK_INIT;
CONDITION_VARIABLE systemWorkCondition = CONDITION_VARIABLE_INIT;
volatile LONG systemWorkState = SystemWorkStateNeverInitialized;
LONG systemWorkCount;
HANDLE rmSemaphore;

SRWLOCK gpuWorkLock = SRWLOCK_INIT;
CONDITION_VARIABLE gpuWorkCondition = CONDITION_VARIABLE_INIT;
LONG gpuWorkCount;
bool gpuWorkClosing;

VOID CALLBACK SystemWorkCallback(PTP_CALLBACK_INSTANCE, void* context)
{
    SystemWorkItem* work = static_cast<SystemWorkItem*>(context);
    work->function(work->context);
    HeapFree(GetProcessHeap(), 0, work);

    AcquireSRWLockExclusive(&systemWorkLock);
    --systemWorkCount;
    if (systemWorkCount == 0)
    {
        WakeAllConditionVariable(&systemWorkCondition);
    }
    ReleaseSRWLockExclusive(&systemWorkLock);
}

} // namespace

extern "C" NV_STATUS NvPlatformRmSystemWorkInitialize(void)
{
    const LONG previousState = InterlockedCompareExchange(
        &systemWorkState,
        SystemWorkStateActive,
        SystemWorkStateNeverInitialized);
    if (previousState == SystemWorkStateNeverInitialized)
    {
        rmSemaphore = CreateSemaphoreW(nullptr, 1, 1, nullptr);
        if (rmSemaphore == nullptr)
        {
            InterlockedExchange(&systemWorkState, SystemWorkStateNeverInitialized);
            return NV_ERR_NO_MEMORY;
        }
        return NV_OK;
    }

    if (previousState == SystemWorkStateStopped &&
        InterlockedCompareExchange(
            &systemWorkState,
            SystemWorkStateActive,
            SystemWorkStateStopped) == SystemWorkStateStopped)
    {
        return NV_OK;
    }

    return previousState == SystemWorkStateActive ? NV_OK : NV_ERR_INVALID_STATE;
}

extern "C" NV_STATUS NvPlatformRmSemaAcquire(void)
{
    if (rmSemaphore == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }
    return WaitForSingleObject(rmSemaphore, INFINITE) == WAIT_OBJECT_0 ?
        NV_OK : NV_ERR_INVALID_STATE;
}

extern "C" NV_STATUS NvPlatformRmSemaTryAcquire(void)
{
    if (rmSemaphore == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }
    return WaitForSingleObject(rmSemaphore, 0) == WAIT_OBJECT_0 ?
        NV_OK : NV_ERR_STATE_IN_USE;
}

extern "C" void NvPlatformRmSemaRelease(void)
{
    if (rmSemaphore != nullptr)
    {
        ReleaseSemaphore(rmSemaphore, 1, nullptr);
    }
}

extern "C" NV_STATUS NvPlatformRmQueueSystemWork(
    NvPlatformRmSystemWorkFunction* function,
    void* context)
{
    if (function == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    SystemWorkItem* work = static_cast<SystemWorkItem*>(HeapAlloc(
        GetProcessHeap(), 0, sizeof(SystemWorkItem)));
    if (work == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }
    work->function = function;
    work->context = context;

    AcquireSRWLockExclusive(&systemWorkLock);
    if (systemWorkState != SystemWorkStateActive)
    {
        ReleaseSRWLockExclusive(&systemWorkLock);
        HeapFree(GetProcessHeap(), 0, work);
        return NV_ERR_INVALID_STATE;
    }
    ++systemWorkCount;
    ReleaseSRWLockExclusive(&systemWorkLock);

    if (!TrySubmitThreadpoolCallback(SystemWorkCallback, work, nullptr))
    {
        AcquireSRWLockExclusive(&systemWorkLock);
        --systemWorkCount;
        if (systemWorkCount == 0)
        {
            WakeAllConditionVariable(&systemWorkCondition);
        }
        ReleaseSRWLockExclusive(&systemWorkLock);
        HeapFree(GetProcessHeap(), 0, work);
        return NV_ERR_OPERATING_SYSTEM;
    }
    return NV_OK;
}

extern "C" NvBool NvPlatformRmSystemWorkIsActive(void)
{
    return systemWorkState == SystemWorkStateActive ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformRmDrainSystemWork(void)
{
    AcquireSRWLockExclusive(&systemWorkLock);
    if (systemWorkState == SystemWorkStateActive)
    {
        systemWorkState = SystemWorkStateStopped;
    }
    while (systemWorkCount != 0)
    {
        SleepConditionVariableSRW(
            &systemWorkCondition, &systemWorkLock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&systemWorkLock);
}

extern "C" NvBool NvPlatformRmGpuWorkAcquire(void)
{
    AcquireSRWLockExclusive(&gpuWorkLock);
    const NvBool acquired = gpuWorkClosing ? NV_FALSE : NV_TRUE;
    if (acquired)
    {
        ++gpuWorkCount;
    }
    ReleaseSRWLockExclusive(&gpuWorkLock);
    return acquired;
}

extern "C" void NvPlatformRmGpuWorkRelease(void)
{
    AcquireSRWLockExclusive(&gpuWorkLock);
    if (gpuWorkCount != 0)
    {
        --gpuWorkCount;
    }
    if (gpuWorkCount == 0)
    {
        WakeAllConditionVariable(&gpuWorkCondition);
    }
    ReleaseSRWLockExclusive(&gpuWorkLock);
}

extern "C" void NvPlatformRmGpuDestroyBegin(void)
{
    AcquireSRWLockExclusive(&gpuWorkLock);
    gpuWorkClosing = true;
    while (gpuWorkCount != 0)
    {
        SleepConditionVariableSRW(
            &gpuWorkCondition, &gpuWorkLock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&gpuWorkLock);
}

extern "C" void NvPlatformRmGpuDestroyEnd(void)
{
    AcquireSRWLockExclusive(&gpuWorkLock);
    gpuWorkClosing = false;
    ReleaseSRWLockExclusive(&gpuWorkLock);
}