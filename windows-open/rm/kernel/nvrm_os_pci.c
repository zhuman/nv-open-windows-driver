#include "core/core.h"
#include "nvplatform_pci_rm.h"

void* osPciInitHandle(
    NvU32 domain,
    NvU8 bus,
    NvU8 slot,
    NvU8 function,
    NvU16* pVendor,
    NvU16* pDevice)
{
    void* handle = NvPlatformRmOpenPciConfig(domain, bus, slot, function);

    if (handle != NULL && (pVendor != NULL || pDevice != NULL))
    {
        NvU32 identity;
        if (NvPlatformRmReadPciConfig(
            handle,
            0,
            &identity,
            sizeof(identity)) != NV_OK)
        {
            return NULL;
        }

        if (pVendor != NULL)
        {
            *pVendor = (NvU16)identity;
        }
        if (pDevice != NULL)
        {
            *pDevice = (NvU16)(identity >> 16);
        }
    }

    return handle;
}

NvU8 osPciReadByte(void* pHandle, NvU32 offset)
{
    NvU8 value = (NvU8)~0U;
    (void)NvPlatformRmReadPciConfig(pHandle, offset, &value, sizeof(value));
    return value;
}

NvU16 osPciReadWord(void* pHandle, NvU32 offset)
{
    NvU16 value = (NvU16)~0U;
    (void)NvPlatformRmReadPciConfig(pHandle, offset, &value, sizeof(value));
    return value;
}

NvU32 osPciReadDword(void* pHandle, NvU32 offset)
{
    NvU32 value = ~0U;
    (void)NvPlatformRmReadPciConfig(pHandle, offset, &value, sizeof(value));
    return value;
}

void osPciWriteByte(void* pHandle, NvU32 offset, NvU8 value)
{
    (void)NvPlatformRmWritePciConfig(pHandle, offset, &value, sizeof(value));
}

void osPciWriteWord(void* pHandle, NvU32 offset, NvU16 value)
{
    (void)NvPlatformRmWritePciConfig(pHandle, offset, &value, sizeof(value));
}

void osPciWriteDword(void* pHandle, NvU32 offset, NvU32 value)
{
    (void)NvPlatformRmWritePciConfig(pHandle, offset, &value, sizeof(value));
}