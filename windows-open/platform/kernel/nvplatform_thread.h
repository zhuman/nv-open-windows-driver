#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NvU64 NvPlatformGetCurrentThreadId(void);
NvU64 NvPlatformGetCurrentProcessId(void);
NvBool NvPlatformIsThreadIdValid(NvU64 threadId);
void NvPlatformYieldThread(void);

typedef NvS32 (*NV_PLATFORM_THREAD_MAIN)(void* argument);

NV_STATUS NvPlatformThreadInitialize(void);
void NvPlatformThreadShutdown(void);
NV_STATUS NvPlatformThreadCreate(
	NvU64* threadId,
	NV_PLATFORM_THREAD_MAIN threadMain,
	void* argument);
void NvPlatformThreadDestroy(NvU64 threadId);
void NvPlatformThreadExit(NvS32 status);
NV_STATUS NvPlatformThreadWait(
	NvU64 threadId,
	NvS32* returnStatus);

#ifdef __cplusplus
}
#endif
