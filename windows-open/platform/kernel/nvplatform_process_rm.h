#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmAttachToProcess(void** processState, NvU32 processId);
void NvPlatformRmDetachFromProcess(void* processState);
NV_STATUS NvPlatformRmValidateKernelBuffer(const void* address, NvU32 size);

#ifdef __cplusplus
}
#endif