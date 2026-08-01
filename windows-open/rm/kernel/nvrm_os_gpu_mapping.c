#include "core/core.h"
#include "g_os_nvoc.h"
#include "gpu/gpu.h"

NV_STATUS osMapPciMemoryAreaUser(
    OS_GPU_INFO* pOsGpuInfo,
    MemoryArea memArea,
    NvU32 protect,
    NvU32 mode,
    NvP64* pVirtualAddress,
    NvP64* pPriv)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS osMapPciMemoryUser(
    OS_GPU_INFO* pOsGpuInfo,
    RmPhysAddr busAddress,
    NvU64 length,
    NvU32 protect,
    NvP64* pVirtualAddress,
    NvP64* pPriv,
    NvU32 mode)
{
    return NV_ERR_NOT_SUPPORTED;
}

void osUnmapPciMemoryUser(
    OS_GPU_INFO* pOsGpuInfo,
    NvP64 virtualAddress,
    NvU64 length,
    NvP64 priv)
{
}

NV_STATUS osMapGPU(
    OBJGPU* pGpu,
    RS_PRIV_LEVEL privLevel,
    NvU64 offset,
    NvU64 length,
    NvU32 protect,
    NvP64* pAddress,
    NvP64* pPriv)
{
    DEVICE_MAPPING* pMapping;

    if (pGpu == NULL || pAddress == NULL || pPriv == NULL || length == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (privLevel < RS_PRIV_LEVEL_KERNEL)
    {
        if (offset > NV_U64_MAX - pGpu->busInfo.gpuPhysAddr)
        {
            return NV_ERR_INVALID_LIMIT;
        }

        return osMapPciMemoryUser(
            pGpu->pOsGpuInfo,
            pGpu->busInfo.gpuPhysAddr + offset,
            length,
            protect,
            pAddress,
            pPriv,
            NV_MEMORY_UNCACHED);
    }

    pMapping = &pGpu->deviceMappings[0];
    if (pMapping->gpuNvAddr == NULL ||
        offset > pMapping->gpuNvLength ||
        length > pMapping->gpuNvLength - offset)
    {
        return NV_ERR_INVALID_LIMIT;
    }

    *pAddress = NV_PTR_TO_NvP64((NvU8*)pMapping->gpuNvAddr + offset);
    *pPriv = NV_PTR_TO_NvP64(NULL);
    return NV_OK;
}

void osUnmapGPU(
    OS_GPU_INFO* pOsGpuInfo,
    RS_PRIV_LEVEL privLevel,
    NvP64 address,
    NvU64 length,
    NvP64 priv)
{
    if (privLevel < RS_PRIV_LEVEL_KERNEL)
    {
        osUnmapPciMemoryUser(pOsGpuInfo, address, length, priv);
    }
}