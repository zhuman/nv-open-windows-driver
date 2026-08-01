#include "g_os_nvoc.h"

NV_STATUS osCallACPI_MXMX(OBJGPU* gpu, NvU32 acpiId, NvU8* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_DDC(
    OBJGPU* gpu,
    NvU32 acpiId,
    NvU8* output,
    NvU32* outputSize,
    NvBool readMultipleBlocks)
{
    (void)gpu;
    (void)acpiId;
    (void)output;
    (void)outputSize;
    (void)readMultipleBlocks;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_MXDS(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_MXDM(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_MXID(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_LRST(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_GPUON(OBJGPU* gpu, NvU32* inOut)
{
    (void)gpu;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_GPUOFF(OBJGPU* gpu, NvU32* inOut)
{
    (void)gpu;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_GPUSTA(OBJGPU* gpu, NvU32* inOut)
{
    (void)gpu;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_MXDS(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_MXMX(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_DOS(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_ROM(OBJGPU* gpu, NvU32* input, NvU32* output)
{
    (void)gpu;
    (void)input;
    (void)output;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_NVHG_DCS(OBJGPU* gpu, NvU32 acpiId, NvU32* inOut)
{
    (void)gpu;
    (void)acpiId;
    (void)inOut;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_DOD(OBJGPU* gpu, NvU32* output, NvU32* outputSize)
{
    (void)gpu;
    (void)output;
    (void)outputSize;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_OPTM_GPUON(OBJGPU* gpu)
{
    (void)gpu;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osCallACPI_DSM(
    OBJGPU* gpu,
    ACPI_DSM_FUNCTION function,
    NvU32 subfunction,
    NvU32* inOut,
    NvU16* size)
{
    (void)gpu;
    (void)function;
    (void)subfunction;
    (void)inOut;
    (void)size;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osGetAcpiRsdpFromUefi(NvU32* rsdpAddress)
{
    (void)rsdpAddress;
    return NV_ERR_NOT_SUPPORTED;
}
