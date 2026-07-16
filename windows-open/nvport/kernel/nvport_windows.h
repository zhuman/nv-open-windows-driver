#pragma once

#include "nvcommon_host.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS nvPortWindowsInitialize(
    const NV_COMMON_HOST_CONTEXT* hostContext);

void nvPortWindowsShutdown(
    const NV_COMMON_HOST_CONTEXT* hostContext);

#ifdef __cplusplus
}
#endif