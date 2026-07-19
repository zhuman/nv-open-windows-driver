#include "nvplatform_common.h"

NvU64 osGetPageSize(void)
{
    return NvPlatformGetPageSize();
}

NvU8 osGetPageShift(void)
{
    NvU64 pageSize = NvPlatformGetPageSize();
    NvU8 pageShift = 0;

    while (pageSize > 1)
    {
        pageSize >>= 1;
        ++pageShift;
    }

    return pageShift;
}

NvU64 osGetSupportedSysmemPageSizeMask(void)
{
    return NvPlatformGetPageSize();
}