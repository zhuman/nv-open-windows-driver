#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformSpinlockCreate(void** handle);
void NvPlatformSpinlockDestroy(void* handle);
NvU64 NvPlatformSpinlockAcquire(void* handle);
void NvPlatformSpinlockRelease(void* handle, NvU64 previousIrql);

NV_STATUS NvPlatformMutexCreate(void** handle);
void NvPlatformMutexDestroy(void* handle);
void NvPlatformMutexAcquire(void* handle);
NvBool NvPlatformMutexTryAcquire(void* handle);
void NvPlatformMutexRelease(void* handle);

NV_STATUS NvPlatformSemaphoreCreate(void** handle, NvU32 initialCount);
void NvPlatformSemaphoreDestroy(void* handle);
void NvPlatformSemaphoreAcquire(void* handle);
NvBool NvPlatformSemaphoreTryAcquire(void* handle);
void NvPlatformSemaphoreRelease(void* handle);

NV_STATUS NvPlatformRwLockCreate(void** handle);
void NvPlatformRwLockDestroy(void* handle);
void NvPlatformRwLockAcquireRead(void* handle);
NvBool NvPlatformRwLockTryAcquireRead(void* handle);
void NvPlatformRwLockAcquireWrite(void* handle);
NvBool NvPlatformRwLockTryAcquireWrite(void* handle);
void NvPlatformRwLockRelease(void* handle);

NvU64 NvPlatformGetCurrentExecutionLevel(void);
NvU64 NvPlatformDisablePreemption(void);
void NvPlatformRestorePreemption(NvU64 previousIrql);

#ifdef __cplusplus
}
#endif