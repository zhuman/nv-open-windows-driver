#pragma once

#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

int NvPlatformRmCpuId(
    int operation,
    int suboperation,
    NvU32* eax,
    NvU32* ebx,
    NvU32* ecx,
    NvU32* edx);

NvU32 NvPlatformGetRmCpuFrequencyMhz(void);

NvU32 NvPlatformGetRmCpuCount(void);

NvU32 NvPlatformGetRmCurrentProcessorNumber(void);

#ifdef __cplusplus
}
#endif