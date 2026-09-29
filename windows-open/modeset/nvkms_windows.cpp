#include "nvkms_windows.h"

#include "nv-kernel-rmapi-ops.h"
#include "nv-gpu-info.h"
#include "nvkms-rmapi.h"
extern "C" {
#include "nvidia-modeset-os-interface.h"
}

#include <class/cl0000.h>
#include <cstdio>
#include <cstring>
#include <windows.h>

struct nvkms_ref_ptr
{
    volatile LONG references;
    void* volatile pointer;
};

struct nvkms_timer_t
{
    PTP_TIMER timer;
    nvkms_timer_proc_t* proc;
    void* dataPtr;
    NvU32 dataU32;
    NvBool isRefPtr;
    volatile LONG canceled;
};

namespace {

SRWLOCK gpuRegistryLock = SRWLOCK_INIT;
nv_gpu_info_t registeredGpu = {};
NvBool gpuRegistered = NV_FALSE;

void CALLBACK NvKmsTimerCallback(
    PTP_CALLBACK_INSTANCE instance,
    void* context,
    PTP_TIMER timerHandle)
{
    auto* const timer = static_cast<nvkms_timer_t*>(context);
    void* dataPtr = timer->dataPtr;

    if (timer->isRefPtr)
    {
        dataPtr = nvkms_dec_ref(static_cast<nvkms_ref_ptr*>(dataPtr));
    }

    if (InterlockedCompareExchange(&timer->canceled, 0, 0) == 0 &&
        dataPtr != nullptr)
    {
        timer->proc(dataPtr, timer->dataU32);
    }

    if (timer->isRefPtr)
    {
        DisassociateCurrentThreadFromCallback(instance);
        CloseThreadpoolTimer(timerHandle);
        HeapFree(GetProcessHeap(), 0, timer);
    }
}

nvkms_timer_t* NvKmsAllocateTimer(
    nvkms_timer_proc_t* proc,
    void* dataPtr,
    NvU32 dataU32,
    NvU64 usec,
    NvBool isRefPtr)
{
    auto* const timer = static_cast<nvkms_timer_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nvkms_timer_t)));
    if (timer == nullptr)
    {
        return nullptr;
    }

    timer->proc = proc;
    timer->dataPtr = dataPtr;
    timer->dataU32 = dataU32;
    timer->isRefPtr = isRefPtr;
    timer->timer = CreateThreadpoolTimer(NvKmsTimerCallback, timer, nullptr);
    if (timer->timer == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, timer);
        return nullptr;
    }

    FILETIME dueTime = {};
    const LONGLONG relative100ns = -static_cast<LONGLONG>(usec == 0 ? 1 : usec * 10);
    ULARGE_INTEGER encodedTime = {};
    encodedTime.QuadPart = static_cast<ULONGLONG>(relative100ns);
    dueTime.dwLowDateTime = encodedTime.LowPart;
    dueTime.dwHighDateTime = encodedTime.HighPart;
    SetThreadpoolTimer(timer->timer, &dueTime, 0, 0);
    return timer;
}

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
NvBool nvKmsModuleLoad(void);
void nvKmsModuleUnload(void);
extern const char* const pNV_KMS_ID = "NVIDIA Windows Open Kernel Mode Setting Driver";
}

extern "C" NvBool nvkms_test_fail_alloc_core_channel(NvKmsFailAllocCoreChannelMethod)
{
    return NV_FALSE;
}

extern "C" NvKmsFrlRateForce nvkms_force_frl_rate(void)
{
    return NVKMS_FRL_RATE_FORCE_NONE;
}

extern "C" NvBool nvkms_conceal_vrr_caps(void) { return NV_FALSE; }
extern "C" NvBool nvkms_enhanced_pcon_support(void) { return NV_FALSE; }
extern "C" NvBool nvkms_output_rounding_fix(void) { return NV_TRUE; }
extern "C" NvBool nvkms_disable_hdmi_frl(void) { return NV_FALSE; }
extern "C" NvBool nvkms_disable_vrr_memclk_switch(void) { return NV_FALSE; }
extern "C" NvBool nvkms_hdmi_deepcolor(void) { return NV_TRUE; }
extern "C" NvBool nvkms_opportunistic_display_sync(void) { return NV_TRUE; }
extern "C" NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void)
{
    return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
}
extern "C" NvBool nvkms_enable_overlay_layers(void) { return NV_TRUE; }
extern "C" NvBool nvkms_debug_logging(void) { return NV_TRUE; }

extern "C" void* nvkms_alloc(size_t size, NvBool zero)
{
    return HeapAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, size);
}

extern "C" void nvkms_free(void* pointer, size_t)
{
    HeapFree(GetProcessHeap(), 0, pointer);
}

extern "C" void* nvkms_memset(void* pointer, NvU8 value, size_t size)
{
    return std::memset(pointer, value, size);
}

extern "C" void* nvkms_memcpy(void* destination, const void* source, size_t size)
{
    return std::memcpy(destination, source, size);
}

extern "C" void* nvkms_memmove(void* destination, const void* source, size_t size)
{
    return std::memmove(destination, source, size);
}

extern "C" int nvkms_memcmp(const void* first, const void* second, size_t size)
{
    return std::memcmp(first, second, size);
}

extern "C" size_t nvkms_strlen(const char* string) { return std::strlen(string); }
extern "C" int nvkms_strcmp(const char* first, const char* second)
{
    return std::strcmp(first, second);
}

extern "C" char* nvkms_strncpy(char* destination, const char* source, size_t size)
{
    return std::strncpy(destination, source, size);
}

extern "C" void nvkms_usleep(NvU64 usec)
{
    if (usec < 1000)
    {
        const NvU64 deadline = nvkms_get_usec() + usec;
        while (nvkms_get_usec() < deadline)
        {
            YieldProcessor();
        }
        return;
    }

    Sleep(static_cast<DWORD>((usec + 999) / 1000));
}

extern "C" NvU64 nvkms_get_usec(void)
{
    LARGE_INTEGER counter = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (static_cast<NvU64>(counter.QuadPart) * 1000000ULL) /
        static_cast<NvU64>(frequency.QuadPart);
}

extern "C" int nvkms_copyin(void* kernelPointer, NvU64 userAddress, size_t size)
{
    if (kernelPointer == nullptr || userAddress == 0)
    {
        return ERROR_INVALID_ADDRESS;
    }
    std::memcpy(kernelPointer, reinterpret_cast<const void*>(userAddress), size);
    return 0;
}

extern "C" int nvkms_copyout(NvU64 userAddress, const void* kernelPointer, size_t size)
{
    if (kernelPointer == nullptr || userAddress == 0)
    {
        return ERROR_INVALID_ADDRESS;
    }
    std::memcpy(reinterpret_cast<void*>(userAddress), kernelPointer, size);
    return 0;
}

extern "C" void nvkms_yield(void) { SwitchToThread(); }
extern "C" void nvkms_dump_stack(void) {}
extern "C" NvBool nvkms_syncpt_op(NvKmsSyncPtOp, NvKmsSyncPtOpParams*)
{
    return NV_FALSE;
}

extern "C" int nvkms_vsnprintf(
    char* string,
    size_t size,
    const char* format,
    va_list arguments)
{
    return std::vsnprintf(string, size, format, arguments);
}

extern "C" int nvkms_snprintf(char* string, size_t size, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int result = nvkms_vsnprintf(string, size, format, arguments);
    va_end(arguments);
    return result;
}

extern "C" void nvkms_log(int level, const char* gpuPrefix, const char* message)
{
    char output[1024] = {};
    const char* const levelPrefix = level == NVKMS_LOG_LEVEL_ERROR ? "ERROR: " :
        level == NVKMS_LOG_LEVEL_WARN ? "WARNING: " : "";
    nvkms_snprintf(output, sizeof(output), "NVKMS: %s%s%s\n", levelPrefix,
                   gpuPrefix == nullptr ? "" : gpuPrefix,
                   message == nullptr ? "" : message);
    OutputDebugStringA(output);
}

extern "C" nvkms_ref_ptr* nvkms_alloc_ref_ptr(void* pointer)
{
    auto* const refPointer = static_cast<nvkms_ref_ptr*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nvkms_ref_ptr)));
    if (refPointer != nullptr)
    {
        refPointer->references = 1;
        refPointer->pointer = pointer;
    }
    return refPointer;
}

extern "C" void nvkms_free_ref_ptr(nvkms_ref_ptr* refPointer)
{
    if (refPointer != nullptr)
    {
        InterlockedExchangePointer(&refPointer->pointer, nullptr);
        nvkms_dec_ref(refPointer);
    }
}

extern "C" void nvkms_inc_ref(nvkms_ref_ptr* refPointer)
{
    InterlockedIncrement(&refPointer->references);
}

extern "C" void* nvkms_dec_ref(nvkms_ref_ptr* refPointer)
{
    void* const pointer = InterlockedCompareExchangePointer(
        &refPointer->pointer, nullptr, nullptr);
    if (InterlockedDecrement(&refPointer->references) == 0)
    {
        HeapFree(GetProcessHeap(), 0, refPointer);
    }
    return pointer;
}

extern "C" nvkms_timer_handle_t* nvkms_alloc_timer(
    nvkms_timer_proc_t* proc,
    void* dataPtr,
    NvU32 dataU32,
    NvU64 usec)
{
    return NvKmsAllocateTimer(proc, dataPtr, dataU32, usec, NV_FALSE);
}

extern "C" NvBool nvkms_alloc_timer_with_ref_ptr(
    nvkms_timer_proc_t* proc,
    nvkms_ref_ptr* refPointer,
    NvU32 dataU32,
    NvU64 usec)
{
    nvkms_inc_ref(refPointer);
    if (NvKmsAllocateTimer(proc, refPointer, dataU32, usec, NV_TRUE) != nullptr)
    {
        return NV_TRUE;
    }
    nvkms_dec_ref(refPointer);
    return NV_FALSE;
}

extern "C" void nvkms_free_timer(nvkms_timer_handle_t* timer)
{
    if (timer == nullptr)
    {
        return;
    }
    InterlockedExchange(&timer->canceled, 1);
    SetThreadpoolTimer(timer->timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(timer->timer, TRUE);
    CloseThreadpoolTimer(timer->timer);
    HeapFree(GetProcessHeap(), 0, timer);
}

extern "C" void nvkms_event_queue_changed(nvkms_per_open_handle_t*, NvBool) {}
extern "C" void* nvkms_get_per_open_data(int) { return nullptr; }

extern "C" NvBool nvkms_open_gpu(NvU32 gpuId, NvBool)
{
    AcquireSRWLockShared(&gpuRegistryLock);
    const NvBool found = gpuRegistered && registeredGpu.gpu_id == gpuId;
    ReleaseSRWLockShared(&gpuRegistryLock);
    return found;
}

extern "C" void nvkms_close_gpu(NvU32, NvBool) {}
extern "C" NvBool nvkms_allow_write_combining(void) { return NV_TRUE; }
extern "C" NvBool nvkms_kernel_supports_syncpts(void) { return NV_FALSE; }
extern "C" NvBool nvkms_fd_is_nvidia_chardev(int) { return NV_FALSE; }
extern "C" nvkms_backlight_device* nvkms_register_backlight(
    NvU32,
    NvU32,
    void*,
    NvU32)
{
    return nullptr;
}
extern "C" void nvkms_unregister_backlight(nvkms_backlight_device*) {}

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

extern "C" NV_STATUS NvKmsWindowsInitialize(void)
{
    return nvKmsModuleLoad() ? NV_OK : NV_ERR_GENERIC;
}

extern "C" void NvKmsWindowsShutdown(void)
{
    nvKmsModuleUnload();
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
