#include "nvport_windows.h"

#include "nvport/nvport.h"
#include "nvplatform_thread.h"

#include <stddef.h>

#define NVPORT_WINDOWS_ALLOCATION_SIGNATURE 0x4E76504F52544D45ULL
#define NVPORT_WINDOWS_ALLOCATION_ALIGNMENT 16U

typedef union NVPORT_WINDOWS_ALLOCATION_HEADER
{
    struct
    {
        NvU64 signature;
        NvBool nonPaged;
    } fields;
    NvU8 alignment[NVPORT_WINDOWS_ALLOCATION_ALIGNMENT];
} NVPORT_WINDOWS_ALLOCATION_HEADER;

static const NV_COMMON_HOST_CONTEXT* hostContext;

static void* AllocateUntracked(NvLength lengthBytes, NvBool nonPaged)
{
    NVPORT_WINDOWS_ALLOCATION_HEADER* header;

    if (hostContext == NULL ||
        lengthBytes == 0 ||
        lengthBytes > (~(NvLength)0 - sizeof(*header)))
    {
        return NULL;
    }

    header = nvCommonHostAllocate(
        hostContext,
        lengthBytes + sizeof(*header),
        nonPaged);
    if (header == NULL)
    {
        return NULL;
    }

    header->fields.signature = NVPORT_WINDOWS_ALLOCATION_SIGNATURE;
    header->fields.nonPaged = nonPaged;
    return header + 1;
}

NV_STATUS nvPortWindowsInitialize(
    const NV_COMMON_HOST_CONTEXT* newHostContext)
{
    NV_STATUS status;

    if (newHostContext == NULL || !newHostContext->initialized)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (hostContext != NULL)
    {
        return NV_ERR_STATE_IN_USE;
    }

    hostContext = newHostContext;
    status = NvPlatformThreadInitialize();
    if (status != NV_OK)
    {
        hostContext = NULL;
        return status;
    }

    status = portInitialize();
    if (status != NV_OK)
    {
        NvPlatformThreadShutdown();
        hostContext = NULL;
    }

    return status;
}

void nvPortWindowsShutdown(
    const NV_COMMON_HOST_CONTEXT* currentHostContext)
{
    if (hostContext == currentHostContext)
    {
        portShutdown();
        NvPlatformThreadShutdown();
        hostContext = NULL;
    }
}

NvU64 nvPortWindowsGetMonotonicTime100ns(void)
{
    return nvCommonHostGetMonotonicTime100ns(hostContext);
}

void nvPortWindowsDebugPrintString(
    const char* string,
    NvLength length)
{
    char buffer[256];

    if (string == NULL)
    {
        return;
    }

    while (length != 0)
    {
        NvLength index;
        NvLength chunkLength = length;

        if (chunkLength >= sizeof(buffer))
        {
            chunkLength = sizeof(buffer) - 1;
        }

        for (index = 0; index < chunkLength; ++index)
        {
            buffer[index] = string[index];
        }
        buffer[chunkLength] = '\0';
        nvCommonHostLog(hostContext, 0, buffer);

        string += chunkLength;
        length -= chunkLength;
    }
}

void* _portMemAllocPagedUntracked(NvLength lengthBytes)
{
    return AllocateUntracked(lengthBytes, NV_FALSE);
}

void* _portMemAllocNonPagedUntracked(NvLength lengthBytes)
{
    return AllocateUntracked(lengthBytes, NV_TRUE);
}

void _portMemFreeUntracked(void* data)
{
    NVPORT_WINDOWS_ALLOCATION_HEADER* header;

    if (data == NULL || hostContext == NULL)
    {
        return;
    }

    header = (NVPORT_WINDOWS_ALLOCATION_HEADER*)data - 1;
    if (header->fields.signature != NVPORT_WINDOWS_ALLOCATION_SIGNATURE)
    {
        nvCommonHostLog(hostContext, 0, "NvPort rejected an invalid allocation\n");
        return;
    }

    header->fields.signature = 0;
    nvCommonHostFree(hostContext, header, header->fields.nonPaged);
}

void* portMemCopy(
    void* destination,
    NvLength destinationSize,
    const void* source,
    NvLength sourceSize)
{
    NvLength index;
    NvU8* destinationBytes = destination;
    const NvU8* sourceBytes = source;

    if (destinationSize == 0)
    {
        return destination;
    }

    if (destination == NULL || source == NULL || sourceSize > destinationSize)
    {
        return NULL;
    }

    for (index = 0; index < sourceSize; ++index)
    {
        destinationBytes[index] = sourceBytes[index];
    }

    return destination;
}

void* portMemCopyAligned(
    void* destination,
    NvLength destinationSize,
    const void* source,
    NvLength sourceSize)
{
    return portMemCopy(destination, destinationSize, source, sourceSize);
}

void* portMemMove(
    void* destination,
    NvLength destinationSize,
    const void* source,
    NvLength sourceSize)
{
    NvLength index;
    NvU8* destinationBytes = destination;
    const NvU8* sourceBytes = source;

    if (destinationSize == 0)
    {
        return destination;
    }

    if (destination == NULL || source == NULL || sourceSize > destinationSize)
    {
        return NULL;
    }

    if ((NvUPtr)destinationBytes <= (NvUPtr)sourceBytes)
    {
        for (index = 0; index < sourceSize; ++index)
        {
            destinationBytes[index] = sourceBytes[index];
        }
    }
    else
    {
        for (index = sourceSize; index != 0; --index)
        {
            destinationBytes[index - 1] = sourceBytes[index - 1];
        }
    }

    return destination;
}

void* portMemSet(void* data, NvU8 value, NvLength lengthBytes)
{
    NvLength index;
    NvU8* bytes = data;

    if (lengthBytes == 0 || data == NULL)
    {
        return data;
    }

    for (index = 0; index < lengthBytes; ++index)
    {
        bytes[index] = value;
    }

    return data;
}

void* portMemSetPattern(
    void* data,
    NvLength lengthBytes,
    const NvU8* pattern,
    NvLength patternBytes)
{
    NvLength index;
    NvU8* bytes = data;

    if (lengthBytes == 0)
    {
        return data;
    }

    if (data == NULL || pattern == NULL || patternBytes == 0)
    {
        return NULL;
    }

    for (index = 0; index < lengthBytes; ++index)
    {
        bytes[index] = pattern[index % patternBytes];
    }

    return data;
}

NvS32 portMemCmp(
    const void* data0,
    const void* data1,
    NvLength lengthBytes)
{
    NvLength index;
    const NvU8* bytes0 = data0;
    const NvU8* bytes1 = data1;

    if (lengthBytes == 0)
    {
        return 0;
    }

    if (data0 == NULL || data1 == NULL)
    {
        return -1;
    }

    for (index = 0; index < lengthBytes; ++index)
    {
        if (bytes0[index] != bytes1[index])
        {
            return (NvS32)bytes0[index] - (NvS32)bytes1[index];
        }
    }

    return 0;
}

NvBool portMemExSafeForPagedAlloc(void)
{
    return nvCommonHostIsAllocationSafe(hostContext, NV_FALSE);
}

NvBool portMemExSafeForNonPagedAlloc(void)
{
    return nvCommonHostIsAllocationSafe(hostContext, NV_TRUE);
}

NvLength portMemExGetPageSize(void)
{
    return 4096;
}

NvU32 portTimeGetSeconds(void)
{
    return (NvU32)(nvCommonHostGetUnixTime100ns(hostContext) / 10000000ULL);
}

NvU64 portTimeGetMilliseconds(void)
{
    return nvCommonHostGetUnixTime100ns(hostContext) / 10000ULL;
}

NvU64 portTimeGetMicroseconds(void)
{
    return nvCommonHostGetUnixTime100ns(hostContext) / 10ULL;
}

NvU64 portTimeGetNanoseconds(void)
{
    return nvCommonHostGetUnixTime100ns(hostContext) * 100ULL;
}

NvU64 portTimeGetUptimeMicroseconds(void)
{
    return nvCommonHostGetMonotonicTime100ns(hostContext) / 10ULL;
}

NvU64 portTimeGetUptimeNanoseconds(void)
{
    return nvCommonHostGetMonotonicTime100ns(hostContext) * 100ULL;
}

NvU64 portTimeGetUptimeNanosecondsHighPrecision(void)
{
    return portTimeGetUptimeNanoseconds();
}

NvU64 portTimeGetUptimeResolution(void)
{
    return 100ULL;
}