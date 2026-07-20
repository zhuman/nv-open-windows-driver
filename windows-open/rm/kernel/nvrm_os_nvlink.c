#include "nvlink_os.h"
#include "nvport/nvport.h"

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