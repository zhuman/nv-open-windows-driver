#include "nvplatform_registry_rm.h"

extern "C" NV_STATUS NvPlatformReadRmRegistryDword(
    const char* valueName,
    NvU32* value)
{
    if (valueName == nullptr || valueName[0] == '\0' || value == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_ERR_OBJECT_NOT_FOUND;
}

extern "C" NV_STATUS NvPlatformWriteRmRegistryDword(
    const char* valueName,
    NvU32)
{
    return valueName != nullptr && valueName[0] != '\0' ?
        NV_ERR_NOT_SUPPORTED : NV_ERR_INVALID_ARGUMENT;
}

extern "C" NV_STATUS NvPlatformReadRmRegistryBinary(
    const char* valueName,
    NvU8*,
    NvU32* dataLength)
{
    if (valueName == nullptr || valueName[0] == '\0' || dataLength == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_ERR_OBJECT_NOT_FOUND;
}

extern "C" NV_STATUS NvPlatformReadRmRegistryString(
    const char* valueName,
    NvU8*,
    NvU32* dataLength)
{
    if (valueName == nullptr || valueName[0] == '\0' || dataLength == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_ERR_OBJECT_NOT_FOUND;
}

extern "C" NV_STATUS NvPlatformEnumerateRmRegistry(
    NV_PLATFORM_RM_REGISTRY_CALLBACK callback,
    void*)
{
    return callback != nullptr ? NV_OK : NV_ERR_INVALID_ARGUMENT;
}