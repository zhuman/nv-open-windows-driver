#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NvU64 NvPlatformGetCurrentThreadId(void);
NvU64 NvPlatformGetCurrentProcessId(void);
void NvPlatformGetCurrentProcessName(char* processName, NvU32 length);
NvU32 NvPlatformGetCurrentProcessFlags(void);
NvBool NvPlatformIsThreadIdValid(NvU64 threadId);
void NvPlatformYieldThread(void);
NV_STATUS NvPlatformScheduleThread(void);

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
NvU64 NvPlatformThreadGetPriority(NvU64 threadId);
void NvPlatformThreadSetPriority(NvU64 threadId, NvU64 priority);
NV_STATUS NvPlatformThreadSetAffinity(
	NvU64 group,
	NvU64 mask,
	NvU64* previousGroup,
	NvU64* previousMask);
NV_STATUS NvPlatformThreadSetAffinityByIndex(
	NvU32 coreIndex,
	NvU64* previousGroup,
	NvU64* previousMask);
NV_STATUS NvPlatformThreadRevertAffinity(
	NvU64 previousGroup,
	NvU64 previousMask);

typedef void (*NV_PLATFORM_PROCESS_NOTIFY_ROUTINE)(
	NvU64 processId,
	NvU32 notifyEvent,
	void* context);

NV_STATUS NvPlatformThreadSetProcessNotifyRoutine(
	NV_PLATFORM_PROCESS_NOTIFY_ROUTINE notifyRoutine,
	void* context,
	NV_PLATFORM_PROCESS_NOTIFY_ROUTINE* previousNotifyRoutine,
	void** previousContext);

#ifdef __cplusplus
}
#endif
