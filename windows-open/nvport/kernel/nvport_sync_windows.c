#define PORT_SYNC_IMPL
#include "nvport/nvport.h"

#include "nvplatform_sync.h"

struct PORT_SPINLOCK
{
    void* handle;
    NvU64 previousIrql;
    PORT_MEM_ALLOCATOR* pAllocator;
};

struct PORT_MUTEX
{
    void* handle;
    PORT_MEM_ALLOCATOR* pAllocator;
};

struct PORT_SEMAPHORE
{
    void* handle;
    PORT_MEM_ALLOCATOR* pAllocator;
};

struct PORT_RWLOCK
{
    void* handle;
    PORT_MEM_ALLOCATOR* pAllocator;
};

NV_STATUS portSyncSpinlockInitialize(PORT_SPINLOCK* spinlock)
{
    if (spinlock == NULL)
    {
        return NV_ERR_INVALID_POINTER;
    }

    spinlock->pAllocator = NULL;
    return NvPlatformSpinlockCreate(&spinlock->handle);
}

void portSyncSpinlockDestroy(PORT_SPINLOCK* spinlock)
{
    NvPlatformSpinlockDestroy(spinlock->handle);
    if (spinlock->pAllocator != NULL)
    {
        PORT_FREE(spinlock->pAllocator, spinlock);
    }
}

void portSyncSpinlockAcquire(PORT_SPINLOCK* spinlock)
{
    spinlock->previousIrql = NvPlatformSpinlockAcquire(spinlock->handle);
}

void portSyncSpinlockRelease(PORT_SPINLOCK* spinlock)
{
    NvPlatformSpinlockRelease(spinlock->handle, spinlock->previousIrql);
}

NV_STATUS portSyncMutexInitialize(PORT_MUTEX* mutex)
{
    if (mutex == NULL)
    {
        return NV_ERR_INVALID_POINTER;
    }

    mutex->pAllocator = NULL;
    return NvPlatformMutexCreate(&mutex->handle);
}

NV_STATUS portSyncExFastMutexInitialize(PORT_MUTEX* mutex)
{
    return portSyncMutexInitialize(mutex);
}

void portSyncMutexDestroy(PORT_MUTEX* mutex)
{
    NvPlatformMutexDestroy(mutex->handle);
    if (mutex->pAllocator != NULL)
    {
        PORT_FREE(mutex->pAllocator, mutex);
    }
}

void portSyncMutexAcquire(PORT_MUTEX* mutex)
{
    NvPlatformMutexAcquire(mutex->handle);
}

NvBool portSyncMutexAcquireConditional(PORT_MUTEX* mutex)
{
    return NvPlatformMutexTryAcquire(mutex->handle);
}

void portSyncMutexRelease(PORT_MUTEX* mutex)
{
    NvPlatformMutexRelease(mutex->handle);
}

NV_STATUS portSyncSemaphoreInitialize(
    PORT_SEMAPHORE* semaphore,
    NvU32 startValue)
{
    if (semaphore == NULL)
    {
        return NV_ERR_INVALID_POINTER;
    }

    semaphore->pAllocator = NULL;
    return NvPlatformSemaphoreCreate(&semaphore->handle, startValue);
}

void portSyncSemaphoreDestroy(PORT_SEMAPHORE* semaphore)
{
    NvPlatformSemaphoreDestroy(semaphore->handle);
    if (semaphore->pAllocator != NULL)
    {
        PORT_FREE(semaphore->pAllocator, semaphore);
    }
}

void portSyncSemaphoreAcquire(PORT_SEMAPHORE* semaphore)
{
    NvPlatformSemaphoreAcquire(semaphore->handle);
}

NvBool portSyncSemaphoreAcquireConditional(PORT_SEMAPHORE* semaphore)
{
    return NvPlatformSemaphoreTryAcquire(semaphore->handle);
}

void portSyncSemaphoreRelease(PORT_SEMAPHORE* semaphore)
{
    NvPlatformSemaphoreRelease(semaphore->handle);
}

NV_STATUS portSyncRwLockInitialize(PORT_RWLOCK* rwLock)
{
    if (rwLock == NULL)
    {
        return NV_ERR_INVALID_POINTER;
    }

    rwLock->pAllocator = NULL;
    return NvPlatformRwLockCreate(&rwLock->handle);
}

void portSyncRwLockDestroy(PORT_RWLOCK* rwLock)
{
    NvPlatformRwLockDestroy(rwLock->handle);
    if (rwLock->pAllocator != NULL)
    {
        PORT_FREE(rwLock->pAllocator, rwLock);
    }
}

void portSyncRwLockAcquireRead(PORT_RWLOCK* rwLock)
{
    NvPlatformRwLockAcquireRead(rwLock->handle);
}

NvBool portSyncRwLockAcquireReadConditional(PORT_RWLOCK* rwLock)
{
    return NvPlatformRwLockTryAcquireRead(rwLock->handle);
}

void portSyncRwLockAcquireWrite(PORT_RWLOCK* rwLock)
{
    NvPlatformRwLockAcquireWrite(rwLock->handle);
}

NvBool portSyncRwLockAcquireWriteConditional(PORT_RWLOCK* rwLock)
{
    return NvPlatformRwLockTryAcquireWrite(rwLock->handle);
}

void portSyncRwLockReleaseRead(PORT_RWLOCK* rwLock)
{
    NvPlatformRwLockRelease(rwLock->handle);
}

void portSyncRwLockReleaseWrite(PORT_RWLOCK* rwLock)
{
    NvPlatformRwLockRelease(rwLock->handle);
}

PORT_MUTEX* portSyncExFastMutexCreate(PORT_MEM_ALLOCATOR* allocator)
{
    return portSyncMutexCreate(allocator);
}

NvBool portSyncExSafeToSleep(void)
{
    return NvPlatformGetCurrentExecutionLevel() <= 1 ? NV_TRUE : NV_FALSE;
}

NvBool portSyncExSafeToWake(void)
{
    return NvPlatformGetCurrentExecutionLevel() <= 2 ? NV_TRUE : NV_FALSE;
}

NvU64 portSyncExGetInterruptLevel(void)
{
    return NvPlatformGetCurrentExecutionLevel();
}

NvU64 portSyncExDisablePreemption(void)
{
    return NvPlatformDisablePreemption();
}

void portSyncExRestorePreemption(NvU64 preemptionState)
{
    NvPlatformRestorePreemption(preemptionState);
}

#define PORT_SYNC_COMMON_DEFINE_SPINLOCK
#define PORT_SYNC_COMMON_DEFINE_MUTEX
#define PORT_SYNC_COMMON_DEFINE_SEMAPHORE
#define PORT_SYNC_COMMON_DEFINE_RWLOCK
#define PORT_SYNC_COMMON_DEFINE_SYNC_INIT
#include "sync_common.h"