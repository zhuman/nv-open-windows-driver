#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmMapPhysical(
    NvU64 physicalAddress,
    NvU64 length,
    NvU32 protect,
    NvU32 cacheType,
    void** virtualAddress);

void NvPlatformRmUnmapPhysical(void* virtualAddress);

#ifdef __cplusplus
}
#endif