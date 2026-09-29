#include "nvkms_windows.h"

#include "nv-kernel-rmapi-ops.h"
#include "nvkms-rmapi.h"

#include <class/cl0000.h>

extern "C" {
void Nv01FreeKernel(NVOS00_PARAMETERS*);
void Nv01AllocMemoryKernel(NVOS02_PARAMETERS*);
void Nv04AllocWithAccessKernel(NVOS64_PARAMETERS*);
void Nv04VidHeapControlKernel(NVOS32_PARAMETERS*);
void Nv04MapMemoryKernel(NVOS33_PARAMETERS*);
void Nv04UnmapMemoryKernel(NVOS34_PARAMETERS*);
void Nv04AllocContextDmaKernel(NVOS39_PARAMETERS*);
void Nv04MapMemoryDmaKernel(NVOS46_PARAMETERS*);
void Nv04UnmapMemoryDmaKernel(NVOS47_PARAMETERS*);
void Nv04BindContextDmaKernel(NVOS49_PARAMETERS*);
void Nv04ControlKernel(NVOS54_PARAMETERS*);
void Nv04DupObjectKernel(NVOS55_PARAMETERS*);
void Nv04ShareKernel(NVOS57_PARAMETERS*);
void Nv04AddVblankCallbackKernel(NVOS61_PARAMETERS*);
}

extern "C" void nvkms_call_rm(void* operations)
{
    auto* const ops = static_cast<nvidia_kernel_rmapi_ops_t*>(operations);
    if (ops == nullptr)
    {
        return;
    }

    switch (ops->op)
    {
        case NV01_FREE:
            Nv01FreeKernel(&ops->params.free);
            break;
        case NV01_ALLOC_MEMORY:
            Nv01AllocMemoryKernel(&ops->params.allocMemory64);
            break;
        case NV04_ALLOC:
            Nv04AllocWithAccessKernel(&ops->params.alloc);
            break;
        case NV04_VID_HEAP_CONTROL:
            Nv04VidHeapControlKernel(ops->params.pVidHeapControl);
            break;
        case NV04_MAP_MEMORY:
            Nv04MapMemoryKernel(&ops->params.mapMemory);
            break;
        case NV04_UNMAP_MEMORY:
            Nv04UnmapMemoryKernel(&ops->params.unmapMemory);
            break;
        case NV04_ALLOC_CONTEXT_DMA:
            Nv04AllocContextDmaKernel(&ops->params.allocContextDma2);
            break;
        case NV04_MAP_MEMORY_DMA:
            Nv04MapMemoryDmaKernel(&ops->params.mapMemoryDma);
            break;
        case NV04_UNMAP_MEMORY_DMA:
            Nv04UnmapMemoryDmaKernel(&ops->params.unmapMemoryDma);
            break;
        case NV04_BIND_CONTEXT_DMA:
            Nv04BindContextDmaKernel(&ops->params.bindContextDma);
            break;
        case NV04_CONTROL:
            Nv04ControlKernel(&ops->params.control);
            break;
        case NV04_DUP_OBJECT:
            Nv04DupObjectKernel(&ops->params.dupObject);
            break;
        case NV04_SHARE:
            Nv04ShareKernel(&ops->params.share);
            break;
        case NV04_ADD_VBLANK_CALLBACK:
            Nv04AddVblankCallbackKernel(&ops->params.addVblankCallback);
            break;
    }
}

extern "C" NV_STATUS NvKmsWindowsProbeRmClient(NvHandle* clientHandle)
{
    if (clientHandle == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *clientHandle = NV01_NULL_OBJECT;
    NV_STATUS status = nvRmApiAlloc(
        NV01_NULL_OBJECT,
        NV01_NULL_OBJECT,
        NV01_NULL_OBJECT,
        NV01_ROOT,
        clientHandle);
    if (status != NV_OK)
    {
        return status;
    }

    status = nvRmApiFree(*clientHandle, *clientHandle, *clientHandle);
    return status;
}
