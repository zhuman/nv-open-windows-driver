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