#include "nvplatform_registry_rm.h"

typedef struct OBJGPU OBJGPU;

NV_STATUS osReadRegistryDwordBase(
    OBJGPU* gpu,
    const char* valueName,
    NvU32* value)
{
    (void)gpu;
    return NvPlatformReadRmRegistryDword(valueName, value);
}

NV_STATUS osReadRegistryDword(
    OBJGPU* gpu,
    const char* valueName,
    NvU32* value)
{
    return osReadRegistryDwordBase(gpu, valueName, value);
}