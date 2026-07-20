#include "core/core.h"
#include "nvplatform_mmio_rm.h"

void* osMapKernelSpace(
    RmPhysAddr start,
    NvU64 size,
    NvU32 mode,
    NvU32 protect)
{
    void* address = NULL;
    return NvPlatformRmMapPhysical(
        start,
        size,
        protect,
        mode,
        &address) == NV_OK
        ? address
        : NULL;
}

void osUnmapKernelSpace(void* address, NvU64 size)
{
    (void)size;
    NvPlatformRmUnmapPhysical(address);
}

NV_STATUS osMapPciMemoryKernelOld(
    OBJGPU* pGpu,
    RmPhysAddr busAddress,
    NvU64 length,
    NvU32 protect,
    void** pVirtualAddress,
    NvU32 mode)
{
    (void)pGpu;
    if (pVirtualAddress == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    // The platform provider tracks the aligned WDK mapping metadata globally,
    // so the legacy ABI does not need Linux's per-GPU mapping list.
    return NvPlatformRmMapPhysical(
        busAddress,
        length,
        protect,
        mode,
        pVirtualAddress);
}

void osUnmapPciMemoryKernelOld(OBJGPU* pGpu, void* virtualAddress)
{
    (void)pGpu;
    NvPlatformRmUnmapPhysical(virtualAddress);
}

NV_STATUS osMapPciMemoryKernel64(
    OBJGPU* pGpu,
    RmPhysAddr busAddress,
    NvU64 length,
    NvU32 protect,
    NvP64* pVirtualAddress,
    NvU32 mode)
{
    void* address = NULL;
    NV_STATUS status;

    (void)pGpu;
    if (pVirtualAddress == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = NvPlatformRmMapPhysical(
        busAddress,
        length,
        protect,
        mode,
        &address);
    *pVirtualAddress = NV_PTR_TO_NvP64(address);
    return status;
}

void osUnmapPciMemoryKernel64(OBJGPU* pGpu, NvP64 virtualAddress)
{
    (void)pGpu;
    NvPlatformRmUnmapPhysical(NvP64_VALUE(virtualAddress));
}