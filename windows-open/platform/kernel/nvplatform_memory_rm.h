#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformCopyFromUser(
    const void* userAddress,
    void* kernelAddress,
    NvLength length);

NV_STATUS NvPlatformCopyToUser(
    const void* kernelAddress,
    void* userAddress,
    NvLength length);

#ifdef __cplusplus
}
#endif