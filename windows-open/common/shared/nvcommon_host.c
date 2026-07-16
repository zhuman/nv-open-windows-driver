#include "nvcommon_host.h"

#include "nvmisc.h"
#include <stddef.h>

NV_STATUS nvCommonHostInitialize(
    NV_COMMON_HOST_CONTEXT* context,
    const NV_COMMON_HOST_SERVICES* services)
{
    if (context == NULL || services == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NVMISC_MEMSET(context, 0, sizeof(*context));

    if (services->version != NV_COMMON_HOST_SERVICES_VERSION ||
        services->size < sizeof(*services) ||
        services->allocate == NULL ||
        services->free == NULL ||
        services->getMonotonicTime100ns == NULL ||
        services->getUnixTime100ns == NULL ||
        services->isAllocationSafe == NULL ||
        services->log == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    context->services = *services;
    context->initialized = NV_TRUE;
    return NV_OK;
}

void nvCommonHostShutdown(NV_COMMON_HOST_CONTEXT* context)
{
    if (context != NULL)
    {
        NVMISC_MEMSET(context, 0, sizeof(*context));
    }
}

void* nvCommonHostAllocate(
    const NV_COMMON_HOST_CONTEXT* context,
    NvU64 length,
    NvBool nonPaged)
{
    if (context == NULL ||
        !context->initialized ||
        length == 0 ||
        !nvCommonHostIsAllocationSafe(context, nonPaged))
    {
        return NULL;
    }

    return context->services.allocate(
        context->services.providerContext,
        length,
        nonPaged);
}

void nvCommonHostFree(
    const NV_COMMON_HOST_CONTEXT* context,
    void* allocation,
    NvBool nonPaged)
{
    if (context != NULL && context->initialized && allocation != NULL)
    {
        context->services.free(
            context->services.providerContext,
            allocation,
            nonPaged);
    }
}

NvU64 nvCommonHostGetMonotonicTime100ns(
    const NV_COMMON_HOST_CONTEXT* context)
{
    if (context == NULL || !context->initialized)
    {
        return 0;
    }

    return context->services.getMonotonicTime100ns(
        context->services.providerContext);
}

NvU64 nvCommonHostGetUnixTime100ns(
    const NV_COMMON_HOST_CONTEXT* context)
{
    if (context == NULL || !context->initialized)
    {
        return 0;
    }

    return context->services.getUnixTime100ns(
        context->services.providerContext);
}

NvBool nvCommonHostIsAllocationSafe(
    const NV_COMMON_HOST_CONTEXT* context,
    NvBool nonPaged)
{
    if (context == NULL || !context->initialized)
    {
        return NV_FALSE;
    }

    return context->services.isAllocationSafe(
        context->services.providerContext,
        nonPaged);
}

void nvCommonHostLog(
    const NV_COMMON_HOST_CONTEXT* context,
    NvU32 level,
    const char* message)
{
    if (context != NULL && context->initialized && message != NULL)
    {
        context->services.log(
            context->services.providerContext,
            level,
            message);
    }
}