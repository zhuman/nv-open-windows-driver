#pragma once

#include "nvcommon_host.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS nvPortWindowsInitialize(
    const NV_COMMON_HOST_CONTEXT* hostContext);

void nvPortWindowsShutdown(
    const NV_COMMON_HOST_CONTEXT* hostContext);

void nvPortWindowsDebugPrintString(
    const char* string,
    NvLength length);

#ifdef __cplusplus
}
#endif