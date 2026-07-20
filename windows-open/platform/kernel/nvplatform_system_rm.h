#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NvBool NvPlatformRmIsNotebook(void);

typedef void NvPlatformRmSystemWorkFunction(void* context);

NV_STATUS NvPlatformRmSystemWorkInitialize(void);

NV_STATUS NvPlatformRmSemaAcquire(void);

NV_STATUS NvPlatformRmSemaTryAcquire(void);

void NvPlatformRmSemaRelease(void);

NV_STATUS NvPlatformRmQueueSystemWork(
	NvPlatformRmSystemWorkFunction* function,
	void* context);

void NvPlatformRmDrainSystemWork(void);

#ifdef __cplusplus
}
#endif