#include "nvplatform_memory_rm.h"

#include <string.h>

namespace {

NV_STATUS CopyMemory(
    const void* source,
    void* destination,
    NvLength length)
{
    if (length == 0)
    {
        return NV_OK;
    }
    if (source == nullptr || destination == nullptr)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    return memcpy_s(destination, length, source, length) == 0 ?
        NV_OK : NV_ERR_INVALID_ADDRESS;
}

} // namespace

extern "C" NV_STATUS NvPlatformCopyFromUser(
    const void* userAddress,
    void* kernelAddress,
    NvLength length)
{
    return CopyMemory(userAddress, kernelAddress, length);
}

extern "C" NV_STATUS NvPlatformCopyToUser(
    const void* kernelAddress,
    void* userAddress,
    NvLength length)
{
    return CopyMemory(kernelAddress, userAddress, length);
}