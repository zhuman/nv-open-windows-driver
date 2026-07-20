#include "nvplatform_memory_rm.h"

#include <ntifs.h>

extern "C" NV_STATUS NvPlatformCopyFromUser(
    const void* userAddress,
    void* kernelAddress,
    NvLength length)
{
    __try
    {
        ProbeForRead(userAddress, length, 1);
        RtlCopyMemory(kernelAddress, userAddress, length);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    return NV_OK;
}

extern "C" NV_STATUS NvPlatformCopyToUser(
    const void* kernelAddress,
    void* userAddress,
    NvLength length)
{
    __try
    {
        ProbeForWrite(userAddress, length, 1);
        RtlCopyMemory(userAddress, kernelAddress, length);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    return NV_OK;
}