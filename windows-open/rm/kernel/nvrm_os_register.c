#include "core/core.h"
#include "g_os_nvoc.h"
#include "gpu/gpu.h"
#include "gpu/gpu_access.h"
#include "nvplatform_mmio_rm.h"

static NvBool isValidRegisterAccess(
    const DEVICE_MAPPING* pMapping,
    NvU32 address,
    NvU32 width)
{
    return pMapping != NULL &&
        pMapping->gpuNvAddr != NULL &&
        (address & (width - 1)) == 0 &&
        width <= pMapping->gpuNvLength &&
        address <= pMapping->gpuNvLength - width;
}

static volatile void* registerAddress(
    DEVICE_MAPPING* pMapping,
    NvU32 address)
{
    return (volatile NvU8*)pMapping->gpuNvAddr + address;
}

void osDevWriteReg008(
    OBJGPU* pGpu,
    DEVICE_MAPPING* pMapping,
    NvU32 address,
    NvV8 value)
{
    (void)pGpu;
    NV_ASSERT_OR_RETURN_VOID(isValidRegisterAccess(pMapping, address, sizeof(value)));
    NvPlatformRmWriteRegister8(registerAddress(pMapping, address), value);
}

void osDevWriteReg016(
    OBJGPU* pGpu,
    DEVICE_MAPPING* pMapping,
    NvU32 address,
    NvV16 value)
{
    (void)pGpu;
    NV_ASSERT_OR_RETURN_VOID(isValidRegisterAccess(pMapping, address, sizeof(value)));
    NvPlatformRmWriteRegister16(registerAddress(pMapping, address), value);
}

void osDevWriteReg032(
    OBJGPU* pGpu,
    DEVICE_MAPPING* pMapping,
    NvU32 address,
    NvV32 value)
{
    NvBool vgpuHandled = NV_FALSE;

    vgpuDevWriteReg032(pGpu, address, value, &vgpuHandled);
    if (vgpuHandled)
    {
        return;
    }

    NV_ASSERT_OR_RETURN_VOID(isValidRegisterAccess(pMapping, address, sizeof(value)));
    NvPlatformRmWriteRegister32(registerAddress(pMapping, address), value);
}

NvU8 osDevReadReg008(
    OBJGPU* pGpu,
    DEVICE_MAPPING* pMapping,
    NvU32 address)
{
    (void)pGpu;
    NV_ASSERT_OR_RETURN(
        isValidRegisterAccess(pMapping, address, sizeof(NvU8)),
        0);
    return NvPlatformRmReadRegister8(registerAddress(pMapping, address));
}

NvU16 osDevReadReg016(
    OBJGPU* pGpu,
    DEVICE_MAPPING* pMapping,
    NvU32 address)
{
    (void)pGpu;
    NV_ASSERT_OR_RETURN(
        isValidRegisterAccess(pMapping, address, sizeof(NvU16)),
        0);
    return NvPlatformRmReadRegister16(registerAddress(pMapping, address));
}

NvU32 osDevReadReg032(
    OBJGPU* pGpu,
    DEVICE_MAPPING* pMapping,
    NvU32 address)
{
    NvBool vgpuHandled = NV_FALSE;
    NvU32 value = vgpuDevReadReg032(pGpu, address, &vgpuHandled);
    if (vgpuHandled)
    {
        return value;
    }

    NV_ASSERT_OR_RETURN(
        isValidRegisterAccess(pMapping, address, sizeof(NvU32)),
        0);
    return NvPlatformRmReadRegister32(registerAddress(pMapping, address));
}