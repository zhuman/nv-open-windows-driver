#pragma once

#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NvU64 NvPlatformGetCurrentThreadId(void);
NvU64 NvPlatformGetCurrentProcessId(void);
NvBool NvPlatformIsThreadIdValid(NvU64 threadId);
void NvPlatformYieldThread(void);

#ifdef __cplusplus
}
#endif
