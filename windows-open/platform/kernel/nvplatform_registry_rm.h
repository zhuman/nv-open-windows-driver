#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NV_PLATFORM_RM_REGISTRY_TYPE
{
    NV_PLATFORM_RM_REGISTRY_DWORD = 1,
    NV_PLATFORM_RM_REGISTRY_BINARY = 2,
    NV_PLATFORM_RM_REGISTRY_STRING = 3
} NV_PLATFORM_RM_REGISTRY_TYPE;

typedef NV_STATUS (*NV_PLATFORM_RM_REGISTRY_CALLBACK)(
    void* context,
    const char* valueName,
    NV_PLATFORM_RM_REGISTRY_TYPE type,
    const NvU8* data,
    NvU32 dataLength);

NV_STATUS NvPlatformReadRmRegistryDword(
    const char* valueName,
    NvU32* value);

NV_STATUS NvPlatformWriteRmRegistryDword(
    const char* valueName,
    NvU32 value);

NV_STATUS NvPlatformReadRmRegistryBinary(
    const char* valueName,
    NvU8* data,
    NvU32* dataLength);

NV_STATUS NvPlatformReadRmRegistryString(
    const char* valueName,
    NvU8* data,
    NvU32* dataLength);

NV_STATUS NvPlatformEnumerateRmRegistry(
    NV_PLATFORM_RM_REGISTRY_CALLBACK callback,
    void* context);

#ifdef __cplusplus
}
#endif