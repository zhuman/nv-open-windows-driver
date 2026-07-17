#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformReadRmRegistryDword(
    const char* valueName,
    NvU32* value);

#ifdef __cplusplus
}
#endif