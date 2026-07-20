#include "nvplatform_crypto_rm.h"
#include "nvplatform_time_rm.h"

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

void libspdm_sleep(NvU64 microseconds)
{
    while (microseconds != 0)
    {
        const NvU32 delay = microseconds > NV_U32_MAX
            ? NV_U32_MAX
            : (NvU32)microseconds;
        if (NvPlatformDelayRmMicroseconds(delay) != NV_OK)
        {
            return;
        }
        microseconds -= delay;
    }
}

NvBool libspdm_encode_base64(
    const NvU8* source,
    NvU8* destination,
    size_t sourceLength,
    size_t* destinationLength)
{
    static const NvU8 alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const size_t maximumSize = (size_t)-1;
    if (destinationLength == NULL ||
        (source == NULL && sourceLength != 0) ||
        sourceLength > maximumSize - 2)
    {
        return NV_FALSE;
    }

    const size_t groupCount = (sourceLength + 2) / 3;
    if (groupCount > maximumSize / 4)
    {
        return NV_FALSE;
    }
    const size_t requiredLength = groupCount * 4;
    if (*destinationLength < requiredLength)
    {
        *destinationLength = requiredLength;
        return NV_FALSE;
    }
    if (destination == NULL && requiredLength != 0)
    {
        return NV_FALSE;
    }

    size_t sourceIndex = 0;
    size_t destinationIndex = 0;
    while (sourceLength - sourceIndex >= 3)
    {
        const NvU32 value = ((NvU32)source[sourceIndex] << 16) |
            ((NvU32)source[sourceIndex + 1] << 8) |
            source[sourceIndex + 2];
        destination[destinationIndex++] = alphabet[(value >> 18) & 0x3f];
        destination[destinationIndex++] = alphabet[(value >> 12) & 0x3f];
        destination[destinationIndex++] = alphabet[(value >> 6) & 0x3f];
        destination[destinationIndex++] = alphabet[value & 0x3f];
        sourceIndex += 3;
    }

    const size_t remaining = sourceLength - sourceIndex;
    if (remaining != 0)
    {
        NvU32 value = (NvU32)source[sourceIndex] << 16;
        if (remaining == 2)
        {
            value |= (NvU32)source[sourceIndex + 1] << 8;
        }
        destination[destinationIndex++] = alphabet[(value >> 18) & 0x3f];
        destination[destinationIndex++] = alphabet[(value >> 12) & 0x3f];
        destination[destinationIndex++] = remaining == 2
            ? alphabet[(value >> 6) & 0x3f]
            : '=';
        destination[destinationIndex++] = '=';
    }

    *destinationLength = destinationIndex;
    return NV_TRUE;
}

NvBool libspdm_asn1_get_tag(
    NvU8** pointer,
    const NvU8* end,
    size_t* length,
    NvU32 tag)
{
    if (pointer == NULL || *pointer == NULL || end == NULL || length == NULL ||
        tag > NV_U8_MAX || *pointer >= end || **pointer != (NvU8)tag)
    {
        return NV_FALSE;
    }

    const NvU8* cursor = *pointer + 1;
    if (cursor >= end)
    {
        return NV_FALSE;
    }

    size_t contentLength = 0;
    size_t headerLength = 2;
    const NvU8 firstLength = *cursor++;
    if ((firstLength & 0x80) == 0)
    {
        contentLength = firstLength;
    }
    else
    {
        const NvU8 lengthBytes = firstLength & 0x7f;
        if (lengthBytes == 0 || lengthBytes > sizeof(size_t) ||
            (size_t)(end - cursor) < lengthBytes || cursor[0] == 0)
        {
            return NV_FALSE;
        }

        headerLength += lengthBytes;
        for (NvU8 index = 0; index < lengthBytes; ++index)
        {
            if (contentLength > ((size_t)-1) >> 8)
            {
                return NV_FALSE;
            }
            contentLength = (contentLength << 8) | cursor[index];
        }
        if (contentLength < 0x80)
        {
            return NV_FALSE;
        }
    }

    if (contentLength > (size_t)(end - *pointer) - headerLength)
    {
        return NV_FALSE;
    }
    *length = headerLength + contentLength;
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