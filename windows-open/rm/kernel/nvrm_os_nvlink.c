#include "nvlink_os.h"
#include "nvport/nvport.h"

typedef struct OBJGPU OBJGPU;
struct nvlink_link_handlers;

NV_STATUS osDelay(NvU32 milliseconds);
void osAssertFailed(void);

void* nvlink_malloc(NvLength length)
{
    return portMemAllocNonPaged(length);
}

void nvlink_free(void* allocation)
{
    portMemFree(allocation);
}

void* nvlink_memset(void* destination, int value, NvLength length)
{
    return portMemSet(destination, (NvU8)value, length);
}

void* nvlink_memcpy(
    void* destination,
    const void* source,
    NvLength length)
{
    return portMemCopy(destination, length, source, length);
}

NvLength nvlink_strlen(const char* string)
{
    return portStringLength(string);
}

void nvlink_assert(int expression)
{
    if (!expression)
    {
        osAssertFailed();
    }
}

void nvlink_sleep(unsigned int milliseconds)
{
    (void)osDelay(milliseconds);
}

NV_STATUS osGetForcedNVLinkConnection(
    OBJGPU* pGpu,
    NvU32 maxLinks,
    NvU32* linkConnection)
{
    (void)pGpu;
    (void)maxLinks;
    (void)linkConnection;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osGetPlatformNvlinkLinerate(
    OBJGPU* pGpu,
    NvU32* lineRate)
{
    (void)pGpu;
    (void)lineRate;
    return NV_ERR_NOT_SUPPORTED;
}

const struct nvlink_link_handlers* osGetNvlinkLinkCallbacks(void)
{
    return NULL;
}