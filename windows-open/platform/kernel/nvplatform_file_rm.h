#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmOpenTemporaryFile(void** file);
void NvPlatformRmCloseFile(void* file);
NV_STATUS NvPlatformRmWriteFile(
    void* file,
    const NvU8* buffer,
    NvU64 size,
    NvU64 offset);
NV_STATUS NvPlatformRmReadFile(
    void* file,
    NvU8* buffer,
    NvU64 size,
    NvU64 offset);

#ifdef __cplusplus
}
#endif