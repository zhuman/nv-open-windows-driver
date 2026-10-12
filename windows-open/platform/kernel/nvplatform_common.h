#pragma once

#include "nvcommon_host.h"

#ifdef __cplusplus
extern "C" {
#endif

const NV_COMMON_HOST_SERVICES* NvPlatformGetCommonHostServices(void);
NvU64 NvPlatformGetPageSize(void);

typedef void (*NV_PLATFORM_DEBUG_LOG)(NvU32 level, const char* message);
void NvPlatformSetDebugLog(NV_PLATFORM_DEBUG_LOG log);

void NvPlatformDebugPrintf(
	NvU32 level,
	const char* format,
	...);

#ifdef __cplusplus
}
#endif