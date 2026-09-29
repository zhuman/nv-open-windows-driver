#include "nvkms_windows.h"

#include "nv-kernel-rmapi-ops.h"
#include "nv-gpu-info.h"
#include "nvkms-rmapi.h"

#include <class/cl0000.h>
#include <windows.h>

namespace {

SRWLOCK gpuRegistryLock = SRWLOCK_INIT;
nv_gpu_info_t registeredGpu = {};
NvBool gpuRegistered = NV_FALSE;

}

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

extern "C" NV_STATUS NvKmsWindowsRegisterGpu(
    NvU32 gpuId,
    NvU32 domain,
    NvU8 bus,
    NvU8 slot,
    NvU8 function,
    void* osDevice)
{
    if (gpuId == 0 || osDevice == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    AcquireSRWLockExclusive(&gpuRegistryLock);
    NV_STATUS status = NV_OK;
    if (gpuRegistered && registeredGpu.gpu_id != gpuId)
    {
        status = NV_ERR_IN_USE;
    }
    else
    {
        registeredGpu.gpu_id = gpuId;
        registeredGpu.pci_info.domain = domain;
        registeredGpu.pci_info.bus = bus;
        registeredGpu.pci_info.slot = slot;
        registeredGpu.pci_info.function = function;
        registeredGpu.needs_numa_setup = NV_FALSE;
        registeredGpu.is_soc_disp = NV_FALSE;
        registeredGpu.os_device_ptr = osDevice;
        gpuRegistered = NV_TRUE;
    }
    ReleaseSRWLockExclusive(&gpuRegistryLock);
    return status;
}

extern "C" void NvKmsWindowsUnregisterGpu(NvU32 gpuId)
{
    AcquireSRWLockExclusive(&gpuRegistryLock);
    if (gpuRegistered && registeredGpu.gpu_id == gpuId)
    {
        registeredGpu = {};
        gpuRegistered = NV_FALSE;
    }
    ReleaseSRWLockExclusive(&gpuRegistryLock);
}

extern "C" NvU32 nvkms_enumerate_gpus(nv_gpu_info_t* gpuInfo)
{
    if (gpuInfo == nullptr)
    {
        return 0;
    }

    AcquireSRWLockShared(&gpuRegistryLock);
    const NvU32 count = gpuRegistered ? 1 : 0;
    if (gpuRegistered)
    {
        gpuInfo[0] = registeredGpu;
    }
    ReleaseSRWLockShared(&gpuRegistryLock);
    return count;
}

extern "C" NV_STATUS NvKmsWindowsProbeGpuEnumeration(
    NV_KMS_WINDOWS_GPU_INFO* gpuInfo)
{
    if (gpuInfo == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    nv_gpu_info_t enumeratedGpus[NV_MAX_GPUS] = {};
    gpuInfo->count = nvkms_enumerate_gpus(enumeratedGpus);
    if (gpuInfo->count == 0)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    gpuInfo->gpuId = enumeratedGpus[0].gpu_id;
    gpuInfo->domain = enumeratedGpus[0].pci_info.domain;
    gpuInfo->bus = enumeratedGpus[0].pci_info.bus;
    gpuInfo->slot = enumeratedGpus[0].pci_info.slot;
    gpuInfo->function = enumeratedGpus[0].pci_info.function;
    return NV_OK;
}
