#include "nvplatform_crypto_rm.h"

#include <stddef.h>

NV_STATUS osGetRandomBytes(NvU8* bytes, NvU16 byteCount)
{
    return NvPlatformRmGetRandomBytes(bytes, byteCount);
}

NvBool libspdm_random_bytes(NvU8* output, size_t size)
{
    if (size > NV_U32_MAX)
    {
        return NV_FALSE;
    }

    return NvPlatformRmGetRandomBytes(output, (NvU32)size) == NV_OK;
}