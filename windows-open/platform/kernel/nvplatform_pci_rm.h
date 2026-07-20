#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

void* NvPlatformRmOpenPciConfig(
    NvU32 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function);

NV_STATUS NvPlatformRmReadPciConfig(
    void* handle,
    NvU32 offset,
    void* buffer,
    NvU32 length);

NV_STATUS NvPlatformRmWritePciConfig(
    void* handle,
    NvU32 offset,
    const void* buffer,
    NvU32 length);

#ifdef __cplusplus
}
#endif