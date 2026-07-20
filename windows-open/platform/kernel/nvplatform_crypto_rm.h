#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmGetRandomBytes(NvU8* bytes, NvU32 byteCount);

#ifdef __cplusplus
}
#endif