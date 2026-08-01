#pragma once

#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

void NvPlatformRmCpuInitialize(void);

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

NvU32 NvPlatformRmReadCr4(void);

NvU64 NvPlatformRmReadXcr0(void);

#ifdef __cplusplus
}
#endif