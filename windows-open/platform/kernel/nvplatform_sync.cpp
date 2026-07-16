#include "nvplatform_sync.h"

extern "C" {
#include <ntddk.h>
}

namespace {

constexpr ULONG NvSyncPoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('S') << 16) |
    (static_cast<ULONG>('y') << 24);

struct SpinlockState
{
    KSPIN_LOCK lock;
};

struct MutexState
{
    KMUTEX mutex;
};

struct SemaphoreState
{
    KSEMAPHORE semaphore;
};

struct RwLockState
{
    ERESOURCE resource;
};

template<typename T>
T* AllocateState() noexcept
{
    return static_cast<T*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(T),
        NvSyncPoolTag));
}

void FreeState(void* state) noexcept
{
    ExFreePoolWithTag(state, NvSyncPoolTag);
}

} // namespace

extern "C" NV_STATUS NvPlatformSpinlockCreate(void** handle)
{
    if (handle == nullptr)
    {
        return NV_ERR_INVALID_POINTER;
    }

    *handle = nullptr;
    auto* state = AllocateState<SpinlockState>();
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    KeInitializeSpinLock(&state->lock);
    *handle = state;
    return NV_OK;
}

extern "C" void NvPlatformSpinlockDestroy(void* handle)
{
    if (handle != nullptr)
    {
        FreeState(handle);
    }
}

extern "C" NvU64 NvPlatformSpinlockAcquire(void* handle)
{
    KIRQL previousIrql;
    auto* state = static_cast<SpinlockState*>(handle);
    KeAcquireSpinLock(&state->lock, &previousIrql);
    return previousIrql;
}

extern "C" void NvPlatformSpinlockRelease(void* handle, NvU64 previousIrql)
{
    auto* state = static_cast<SpinlockState*>(handle);
    KeReleaseSpinLock(&state->lock, static_cast<KIRQL>(previousIrql));
}

extern "C" NV_STATUS NvPlatformMutexCreate(void** handle)
{
    if (handle == nullptr)
    {
        return NV_ERR_INVALID_POINTER;
    }

    *handle = nullptr;
    auto* state = AllocateState<MutexState>();
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    KeInitializeMutex(&state->mutex, 0);
    *handle = state;
    return NV_OK;
}

extern "C" void NvPlatformMutexDestroy(void* handle)
{
    if (handle != nullptr)
    {
        FreeState(handle);
    }
}

extern "C" void NvPlatformMutexAcquire(void* handle)
{
    auto* state = static_cast<MutexState*>(handle);
    (void)KeWaitForSingleObject(&state->mutex, Executive, KernelMode, FALSE, nullptr);
}

extern "C" NvBool NvPlatformMutexTryAcquire(void* handle)
{
    LARGE_INTEGER timeout{};
    auto* state = static_cast<MutexState*>(handle);
    return KeWaitForSingleObject(
        &state->mutex,
        Executive,
        KernelMode,
        FALSE,
        &timeout) == STATUS_SUCCESS ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformMutexRelease(void* handle)
{
    auto* state = static_cast<MutexState*>(handle);
    KeReleaseMutex(&state->mutex, FALSE);
}

extern "C" NV_STATUS NvPlatformSemaphoreCreate(void** handle, NvU32 initialCount)
{
    if (handle == nullptr || initialCount > MAXLONG)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *handle = nullptr;
    auto* state = AllocateState<SemaphoreState>();
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    KeInitializeSemaphore(
        &state->semaphore,
        static_cast<LONG>(initialCount),
        MAXLONG);
    *handle = state;
    return NV_OK;
}

extern "C" void NvPlatformSemaphoreDestroy(void* handle)
{
    if (handle != nullptr)
    {
        FreeState(handle);
    }
}

extern "C" void NvPlatformSemaphoreAcquire(void* handle)
{
    auto* state = static_cast<SemaphoreState*>(handle);
    (void)KeWaitForSingleObject(&state->semaphore, Executive, KernelMode, FALSE, nullptr);
}

extern "C" NvBool NvPlatformSemaphoreTryAcquire(void* handle)
{
    LARGE_INTEGER timeout{};
    auto* state = static_cast<SemaphoreState*>(handle);
    return KeWaitForSingleObject(
        &state->semaphore,
        Executive,
        KernelMode,
        FALSE,
        &timeout) == STATUS_SUCCESS ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformSemaphoreRelease(void* handle)
{
    auto* state = static_cast<SemaphoreState*>(handle);
    KeReleaseSemaphore(&state->semaphore, IO_NO_INCREMENT, 1, FALSE);
}

extern "C" NV_STATUS NvPlatformRwLockCreate(void** handle)
{
    NTSTATUS status;

    if (handle == nullptr)
    {
        return NV_ERR_INVALID_POINTER;
    }

    *handle = nullptr;
    auto* state = AllocateState<RwLockState>();
    if (state == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    status = ExInitializeResourceLite(&state->resource);
    if (!NT_SUCCESS(status))
    {
        FreeState(state);
        return NV_ERR_NO_MEMORY;
    }

    *handle = state;
    return NV_OK;
}

extern "C" void NvPlatformRwLockDestroy(void* handle)
{
    if (handle != nullptr)
    {
        auto* state = static_cast<RwLockState*>(handle);
        ExDeleteResourceLite(&state->resource);
        FreeState(state);
    }
}

extern "C" void NvPlatformRwLockAcquireRead(void* handle)
{
    auto* state = static_cast<RwLockState*>(handle);
    (void)ExAcquireResourceSharedLite(&state->resource, TRUE);
}

extern "C" NvBool NvPlatformRwLockTryAcquireRead(void* handle)
{
    auto* state = static_cast<RwLockState*>(handle);
    return ExAcquireResourceSharedLite(&state->resource, FALSE) ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformRwLockAcquireWrite(void* handle)
{
    auto* state = static_cast<RwLockState*>(handle);
    (void)ExAcquireResourceExclusiveLite(&state->resource, TRUE);
}

extern "C" NvBool NvPlatformRwLockTryAcquireWrite(void* handle)
{
    auto* state = static_cast<RwLockState*>(handle);
    return ExAcquireResourceExclusiveLite(&state->resource, FALSE) ? NV_TRUE : NV_FALSE;
}

extern "C" void NvPlatformRwLockRelease(void* handle)
{
    auto* state = static_cast<RwLockState*>(handle);
    ExReleaseResourceLite(&state->resource);
}

extern "C" NvU64 NvPlatformGetCurrentExecutionLevel(void)
{
    return KeGetCurrentIrql();
}

extern "C" void NvPlatformDelayMilliseconds(NvU32 milliseconds)
{
    LARGE_INTEGER interval{};
    interval.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000LL;
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

extern "C" NvU64 NvPlatformDisablePreemption(void)
{
    const KIRQL currentIrql = KeGetCurrentIrql();
    return currentIrql < DISPATCH_LEVEL ? KeRaiseIrqlToDpcLevel() : currentIrql;
}

extern "C" void NvPlatformRestorePreemption(NvU64 previousIrql)
{
    const KIRQL currentIrql = KeGetCurrentIrql();
    const auto targetIrql = static_cast<KIRQL>(previousIrql);
    if (targetIrql < currentIrql)
    {
        KeLowerIrql(targetIrql);
    }
}