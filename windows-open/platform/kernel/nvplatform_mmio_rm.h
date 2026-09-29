#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmMapPhysical(
    NvU64 physicalAddress,
    NvU64 length,
    NvU32 protect,
    NvU32 cacheType,
    void** virtualAddress);

void NvPlatformRmUnmapPhysical(void* virtualAddress);

NvU8 NvPlatformRmReadRegister8(const volatile void* address);
NvU16 NvPlatformRmReadRegister16(const volatile void* address);
NvU32 NvPlatformRmReadRegister32(const volatile void* address);

NV_STATUS NvPlatformRmBeginRegisterCache32(
    const volatile void* address,
    NvU32 count);
void NvPlatformRmEndRegisterCache32(const volatile void* address);

void NvPlatformRmWriteRegister8(volatile void* address, NvU8 value);
void NvPlatformRmWriteRegister16(volatile void* address, NvU16 value);
void NvPlatformRmWriteRegister32(volatile void* address, NvU32 value);

NvU64 NvPlatformRmGetMaximumUserAddress(void);

#ifdef __cplusplus
}
#endif