#include <windows.h>
#include <intrin.h>

#include "nvplatform_sync.h"

namespace {

enum class LockKind
{
    CriticalSection,
    SlimReaderWriter,
};

struct UserRwLock
{
    CRITICAL_SECTION stateLock;
    CONDITION_VARIABLE condition;
    ULONG readerCount;
    DWORD writerThreadId;
};

struct UserLock
{
    LockKind kind;
    union
    {
        CRITICAL_SECTION criticalSection;
        UserRwLock readerWriter;
    } lock;
};

UserLock* AllocateLock(LockKind kind) noexcept
{
    auto* lock = static_cast<UserLock*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UserLock)));
    if (lock == nullptr)
    {
        return nullptr;
    }

    lock->kind = kind;
    if (kind == LockKind::CriticalSection)
    {
        InitializeCriticalSection(&lock->lock.criticalSection);
    }
    else
    {
        InitializeCriticalSection(&lock->lock.readerWriter.stateLock);
        InitializeConditionVariable(&lock->lock.readerWriter.condition);
    }
    return lock;
}

void FreeLock(void* handle) noexcept
{
    auto* lock = static_cast<UserLock*>(handle);
    if (lock == nullptr)
    {
        return;
    }
    if (lock->kind == LockKind::CriticalSection)
    {
        DeleteCriticalSection(&lock->lock.criticalSection);
    }
    else
    {
        DeleteCriticalSection(&lock->lock.readerWriter.stateLock);
    }
    HeapFree(GetProcessHeap(), 0, lock);
}

} // namespace

extern "C" NV_STATUS NvPlatformSpinlockCreate(void** handle)
{
    if (handle == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *handle = AllocateLock(LockKind::CriticalSection);
    return *handle != nullptr ? NV_OK : NV_ERR_NO_MEMORY;
}

extern "C" void NvPlatformSpinlockDestroy(void* handle)
{
    FreeLock(handle);
}

extern "C" NvU64 NvPlatformSpinlockAcquire(void* handle)
{
    EnterCriticalSection(&static_cast<UserLock*>(handle)->lock.criticalSection);
    return 0;
}

extern "C" void NvPlatformSpinlockRelease(void* handle, NvU64 previousIrql)
{
    UNREFERENCED_PARAMETER(previousIrql);
    LeaveCriticalSection(&static_cast<UserLock*>(handle)->lock.criticalSection);
}

extern "C" NV_STATUS NvPlatformMutexCreate(void** handle)
{
    return NvPlatformSpinlockCreate(handle);
}

extern "C" void NvPlatformMutexDestroy(void* handle)
{
    FreeLock(handle);
}

extern "C" void NvPlatformMutexAcquire(void* handle)
{
    EnterCriticalSection(&static_cast<UserLock*>(handle)->lock.criticalSection);
}

extern "C" NvBool NvPlatformMutexTryAcquire(void* handle)
{
    return TryEnterCriticalSection(
        &static_cast<UserLock*>(handle)->lock.criticalSection)
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" void NvPlatformMutexRelease(void* handle)
{
    LeaveCriticalSection(&static_cast<UserLock*>(handle)->lock.criticalSection);
}

extern "C" NV_STATUS NvPlatformSemaphoreCreate(void** handle, NvU32 initialCount)
{
    if (handle == nullptr || initialCount > MAXLONG)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *handle = CreateSemaphoreW(nullptr, initialCount, MAXLONG, nullptr);
    return *handle != nullptr ? NV_OK : NV_ERR_NO_MEMORY;
}

extern "C" void NvPlatformSemaphoreDestroy(void* handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
    }
}

extern "C" void NvPlatformSemaphoreAcquire(void* handle)
{
    (void)WaitForSingleObject(handle, INFINITE);
}

extern "C" NvBool NvPlatformSemaphoreTryAcquire(void* handle)
{
    return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0 ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformSemaphoreRelease(void* handle)
{
    (void)ReleaseSemaphore(handle, 1, nullptr);
}

extern "C" NV_STATUS NvPlatformRwLockCreate(void** handle)
{
    if (handle == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *handle = AllocateLock(LockKind::SlimReaderWriter);
    return *handle != nullptr ? NV_OK : NV_ERR_NO_MEMORY;
}

extern "C" void NvPlatformRwLockDestroy(void* handle)
{
    FreeLock(handle);
}

extern "C" void NvPlatformRwLockAcquireRead(void* handle)
{
    UserRwLock* lock = &static_cast<UserLock*>(handle)->lock.readerWriter;
    EnterCriticalSection(&lock->stateLock);
    while (lock->writerThreadId != 0)
    {
        SleepConditionVariableCS(&lock->condition, &lock->stateLock, INFINITE);
    }
    ++lock->readerCount;
    LeaveCriticalSection(&lock->stateLock);
}

extern "C" NvBool NvPlatformRwLockTryAcquireRead(void* handle)
{
    UserRwLock* lock = &static_cast<UserLock*>(handle)->lock.readerWriter;
    EnterCriticalSection(&lock->stateLock);
    const NvBool acquired = lock->writerThreadId == 0 ? NV_TRUE : NV_FALSE;
    if (acquired)
    {
        ++lock->readerCount;
    }
    LeaveCriticalSection(&lock->stateLock);
    return acquired;
}

extern "C" void NvPlatformRwLockAcquireWrite(void* handle)
{
    UserRwLock* lock = &static_cast<UserLock*>(handle)->lock.readerWriter;
    EnterCriticalSection(&lock->stateLock);
    while (lock->writerThreadId != 0 || lock->readerCount != 0)
    {
        SleepConditionVariableCS(&lock->condition, &lock->stateLock, INFINITE);
    }
    lock->writerThreadId = GetCurrentThreadId();
    LeaveCriticalSection(&lock->stateLock);
}

extern "C" NvBool NvPlatformRwLockTryAcquireWrite(void* handle)
{
    UserRwLock* lock = &static_cast<UserLock*>(handle)->lock.readerWriter;
    EnterCriticalSection(&lock->stateLock);
    const NvBool acquired =
        lock->writerThreadId == 0 && lock->readerCount == 0 ? NV_TRUE : NV_FALSE;
    if (acquired)
    {
        lock->writerThreadId = GetCurrentThreadId();
    }
    LeaveCriticalSection(&lock->stateLock);
    return acquired;
}

extern "C" void NvPlatformRwLockRelease(void* handle)
{
    UserRwLock* lock = &static_cast<UserLock*>(handle)->lock.readerWriter;
    EnterCriticalSection(&lock->stateLock);
    if (lock->writerThreadId == GetCurrentThreadId())
    {
        lock->writerThreadId = 0;
    }
    else if (lock->readerCount != 0)
    {
        --lock->readerCount;
    }
    WakeAllConditionVariable(&lock->condition);
    LeaveCriticalSection(&lock->stateLock);
}

extern "C" NvU64 NvPlatformGetCurrentExecutionLevel(void)
{
    return 0;
}

extern "C" NvBool NvPlatformIsRaisedExecutionLevel(void)
{
    return NV_FALSE;
}

extern "C" NvBool NvPlatformIsInterruptExecutionLevel(void)
{
    return NV_FALSE;
}

extern "C" void NvPlatformProcessorPause(void)
{
    YieldProcessor();
}

extern "C" NvBool NvPlatformFlushCpuCaches(void)
{
    FlushProcessWriteBuffers();
    return NV_TRUE;
}

extern "C" void NvPlatformFlushCpuCacheRange(void* address, NvU64 size)
{
    UNREFERENCED_PARAMETER(address);
    UNREFERENCED_PARAMETER(size);
    (void)FlushProcessWriteBuffers();
}

extern "C" void NvPlatformFlushWriteBuffer(void)
{
    MemoryBarrier();
}

extern "C" void NvPlatformDelayMilliseconds(NvU32 milliseconds)
{
    Sleep(milliseconds);
}

extern "C" NvU64 NvPlatformDisablePreemption(void)
{
    return 0;
}

extern "C" void NvPlatformRestorePreemption(NvU64 previousIrql)
{
    UNREFERENCED_PARAMETER(previousIrql);
}