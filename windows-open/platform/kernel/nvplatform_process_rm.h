#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmAttachToProcess(void** processState, NvU32 processId);
void NvPlatformRmDetachFromProcess(void* processState);
NV_STATUS NvPlatformRmValidateKernelBuffer(const void* address, NvU32 size);
void* NvPlatformRmReferenceCurrentProcess(void);
void NvPlatformRmDereferenceProcess(void* process);
NV_STATUS NvPlatformRmGetProcessId(void* process, NvU32* processId);
NV_STATUS NvPlatformRmGetCurrentProcessId(NvU32* processId);
NV_STATUS NvPlatformRmGetCurrentUserSid(NvU8* sid, NvU32* sidLength);
NvBool NvPlatformRmHasSystemProfilePrivilege(void);
NvBool NvPlatformRmHasIncreaseBasePriorityPrivilege(void);

#ifdef __cplusplus
}
#endif