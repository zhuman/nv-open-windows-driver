#include "nvplatform_file_rm.h"

extern "C" NV_STATUS NvPlatformRmOpenTemporaryFile(void** file)
{
    if (file == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *file = nullptr;
    return NV_ERR_NOT_SUPPORTED;
}

extern "C" void NvPlatformRmCloseFile(void*)
{
}

extern "C" NV_STATUS NvPlatformRmWriteFile(
    void* file,
    const NvU8* buffer,
    NvU64 size,
    NvU64)
{
    if (file == nullptr || (buffer == nullptr && size != 0))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_ERR_NOT_SUPPORTED;
}

extern "C" NV_STATUS NvPlatformRmReadFile(
    void* file,
    NvU8* buffer,
    NvU64 size,
    NvU64)
{
    if (file == nullptr || (buffer == nullptr && size != 0))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return NV_ERR_NOT_SUPPORTED;
}