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

NvBool libspdm_check_crypto_backend(void)
{
    return NV_TRUE;
}

static NvBool decodeBase64Character(
    NvU8 character,
    NvU8* value,
    NvBool* padding)
{
    *padding = NV_FALSE;
    if (character >= 'A' && character <= 'Z')
    {
        *value = character - 'A';
    }
    else if (character >= 'a' && character <= 'z')
    {
        *value = character - 'a' + 26;
    }
    else if (character >= '0' && character <= '9')
    {
        *value = character - '0' + 52;
    }
    else if (character == '+' || character == '-')
    {
        *value = 62;
    }
    else if (character == '/' || character == '_')
    {
        *value = 63;
    }
    else if (character == '=')
    {
        *value = 0;
        *padding = NV_TRUE;
    }
    else
    {
        return NV_FALSE;
    }

    return NV_TRUE;
}

NvBool libspdm_decode_base64(
    const NvU8* source,
    NvU8* destination,
    size_t sourceLength,
    size_t* destinationLength)
{
    if (source == NULL || destination == NULL || destinationLength == NULL)
    {
        return NV_FALSE;
    }

    const size_t capacity = *destinationLength;
    size_t produced = 0;
    NvU8 values[4];
    NvBool padding[4];
    NvU32 quartetLength = 0;
    NvBool finalQuartet = NV_FALSE;

    for (size_t index = 0; index < sourceLength; ++index)
    {
        const NvU8 character = source[index];
        if (character == ' ' || character == '\t' ||
            character == '\r' || character == '\n')
        {
            continue;
        }
        if (finalQuartet || !decodeBase64Character(
            character,
            &values[quartetLength],
            &padding[quartetLength]))
        {
            goto failure;
        }

        ++quartetLength;
        if (quartetLength != 4)
        {
            continue;
        }

        if (padding[0] || padding[1] || (padding[2] && !padding[3]))
        {
            goto failure;
        }

        size_t bytes = 3;
        if (padding[2])
        {
            if ((values[1] & 0x0f) != 0)
            {
                goto failure;
            }
            bytes = 1;
            finalQuartet = NV_TRUE;
        }
        else if (padding[3])
        {
            if ((values[2] & 0x03) != 0)
            {
                goto failure;
            }
            bytes = 2;
            finalQuartet = NV_TRUE;
        }

        if (bytes > capacity - produced)
        {
            goto failure;
        }

        destination[produced++] = (values[0] << 2) | (values[1] >> 4);
        if (bytes > 1)
        {
            destination[produced++] = (values[1] << 4) | (values[2] >> 2);
        }
        if (bytes > 2)
        {
            destination[produced++] = (values[2] << 6) | values[3];
        }
        quartetLength = 0;
    }

    if (quartetLength != 0)
    {
        goto failure;
    }

    *destinationLength = produced;
    return NV_TRUE;

failure:
    *destinationLength = 0;
    return NV_FALSE;
}