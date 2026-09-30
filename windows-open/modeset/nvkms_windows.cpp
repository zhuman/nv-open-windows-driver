#include "nvkms_windows.h"

#include "nv-kernel-rmapi-ops.h"
#include "nv-gpu-info.h"
#include "nvkms-rmapi.h"
extern "C" {
#include "nvidia-modeset-os-interface.h"
#include "nvkms-kapi.h"
#include "nvrm_gpu_attach.h"
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

struct nvkms_per_open
{
    void* data;
    NvKmsKapiDevice* device;
};

struct nvkms_sema_t
{
    HANDLE semaphore;
};

struct NV_KMS_WINDOWS_SURFACE
{
    NvKmsKapiMemory* memory;
    NvKmsKapiSurface* surface;
    void* pixels;
    NvU64 size;
    NvU32 width;
    NvU32 height;
    NvU32 pitch;
};

namespace {

SRWLOCK gpuRegistryLock = SRWLOCK_INIT;
nv_gpu_info_t registeredGpu = {};
NvBool gpuRegistered = NV_FALSE;
NvKmsKapiFunctionsTable kmsFunctions = {};
NvKmsKapiDevice* kmsDevice = nullptr;
NvKmsKapiGpuInfo enumeratedKmsGpu = {};
NvBool kmsGpuEnumerated = NV_FALSE;
SRWLOCK kmsDeviceLock = SRWLOCK_INIT;
INIT_ONCE kmsLockInitOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION kmsLock;

class KmsDeviceLockGuard
{
public:
    KmsDeviceLockGuard() { AcquireSRWLockExclusive(&kmsDeviceLock); }
    ~KmsDeviceLockGuard() { ReleaseSRWLockExclusive(&kmsDeviceLock); }

    KmsDeviceLockGuard(const KmsDeviceLockGuard&) = delete;
    KmsDeviceLockGuard& operator=(const KmsDeviceLockGuard&) = delete;
};

BOOL CALLBACK NvKmsInitializeLock(PINIT_ONCE, void*, void**)
{
    InitializeCriticalSection(&kmsLock);
    return TRUE;
}

void NvKmsLock(void)
{
    InitOnceExecuteOnce(&kmsLockInitOnce, NvKmsInitializeLock, nullptr, nullptr);
    EnterCriticalSection(&kmsLock);
}

void NvKmsUnlock(void)
{
    LeaveCriticalSection(&kmsLock);
}

void NvKmsGpuCallback(const NvKmsKapiGpuInfo* info)
{
    if (info != nullptr && info->gpuInfo.gpu_id == registeredGpu.gpu_id)
    {
        enumeratedKmsGpu = *info;
        kmsGpuEnumerated = NV_TRUE;
    }
}

DWORD WINAPI NvKmsTimerCleanup(void* context)
{
    auto* const timer = static_cast<nvkms_timer_t*>(context);
    WaitForThreadpoolTimerCallbacks(timer->timer, FALSE);
    CloseThreadpoolTimer(timer->timer);
    HeapFree(GetProcessHeap(), 0, timer);
    return 0;
}

void CALLBACK NvKmsTimerCallback(
    PTP_CALLBACK_INSTANCE instance,
    void* context,
    PTP_TIMER timerHandle)
{
    auto* const timer = static_cast<nvkms_timer_t*>(context);
    nvkms_timer_proc_t* const proc = timer->proc;
    void* dataPtr = timer->dataPtr;
    const NvU32 dataU32 = timer->dataU32;
    const NvBool isRefPtr = timer->isRefPtr;

    NvKmsLock();
    if (isRefPtr)
    {
        dataPtr = nvkms_dec_ref(static_cast<nvkms_ref_ptr*>(dataPtr));
    }
    if (InterlockedCompareExchange(&timer->canceled, 0, 0) == 0 &&
        dataPtr != nullptr)
    {
        proc(dataPtr, dataU32);
    }
    NvKmsUnlock();

    if (isRefPtr)
    {
        QueueUserWorkItem(NvKmsTimerCleanup, timer, WT_EXECUTEONLYONCE);
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
void NvKmsWindowsTraceMessage(const char* message);
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
    NvKmsWindowsTraceMessage(output);
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
    QueueUserWorkItem(NvKmsTimerCleanup, timer, WT_EXECUTEONLYONCE);
}

extern "C" nvkms_per_open* nvkms_open_from_kapi(NvKmsKapiDevice* device)
{
    auto* const perOpen = static_cast<nvkms_per_open*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nvkms_per_open)));
    if (perOpen == nullptr)
    {
        return nullptr;
    }

    perOpen->device = device;
    NvKmsLock();
    perOpen->data = nvKmsOpen(
        GetCurrentProcessId(), NVKMS_CLIENT_KERNEL_SPACE, perOpen);
    NvKmsUnlock();
    if (perOpen->data == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, perOpen);
        return nullptr;
    }
    return perOpen;
}

extern "C" void nvkms_close_from_kapi(nvkms_per_open* perOpen)
{
    if (perOpen == nullptr)
    {
        return;
    }
    NvKmsLock();
    nvKmsClose(perOpen->data);
    NvKmsUnlock();
    HeapFree(GetProcessHeap(), 0, perOpen);
}

extern "C" NvBool nvkms_ioctl_from_kapi(
    nvkms_per_open* perOpen,
    NvU32 command,
    void* parameters,
    size_t parameterSize)
{
    if (perOpen == nullptr)
    {
        return NV_FALSE;
    }
    NvKmsLock();
    const NvBool result = nvKmsIoctl(
        perOpen->data,
        command,
        static_cast<NvU64>(reinterpret_cast<NvUPtr>(parameters)),
        parameterSize);
    NvKmsUnlock();
    return result;
}

extern "C" NvBool nvkms_ioctl_from_kapi_try_pmlock(
    nvkms_per_open* perOpen,
    NvU32 command,
    void* parameters,
    size_t parameterSize)
{
    return nvkms_ioctl_from_kapi(perOpen, command, parameters, parameterSize);
}

extern "C" nvkms_sema_handle_t* nvkms_sema_alloc(void)
{
    auto* const semaphore = static_cast<nvkms_sema_t*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nvkms_sema_t)));
    if (semaphore == nullptr)
    {
        return nullptr;
    }
    semaphore->semaphore = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    if (semaphore->semaphore == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, semaphore);
        return nullptr;
    }
    return semaphore;
}

extern "C" void nvkms_sema_free(nvkms_sema_handle_t* semaphore)
{
    if (semaphore != nullptr)
    {
        CloseHandle(semaphore->semaphore);
        HeapFree(GetProcessHeap(), 0, semaphore);
    }
}

extern "C" void nvkms_sema_down(nvkms_sema_handle_t* semaphore)
{
    WaitForSingleObject(semaphore->semaphore, INFINITE);
}

extern "C" void nvkms_sema_up(nvkms_sema_handle_t* semaphore)
{
    ReleaseSemaphore(semaphore->semaphore, 1, nullptr);
}

extern "C" void nvkms_event_queue_changed(
    nvkms_per_open_handle_t* perOpen,
    NvBool eventsAvailable)
{
    if (eventsAvailable && perOpen != nullptr && perOpen->device != nullptr)
    {
        nvKmsKapiHandleEventQueueChange(perOpen->device);
    }
}
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

    char traceMessage[96] = {};
    if (ops->op == NV04_CONTROL)
    {
        std::snprintf(traceMessage, sizeof(traceMessage),
                      "NVKMS RM control begin: cmd=0x%08X",
                      ops->params.control.cmd);
    }
    else if (ops->op == NV04_ALLOC)
    {
        std::snprintf(traceMessage, sizeof(traceMessage),
                      "NVKMS RM alloc begin: class=0x%08X",
                      ops->params.alloc.hClass);
    }
    else
    {
        std::snprintf(traceMessage, sizeof(traceMessage),
                      "NVKMS RM operation begin: op=0x%08X", ops->op);
    }
    NvKmsWindowsTraceMessage(traceMessage);

    void* gpuOsInfo = nullptr;
    AcquireSRWLockShared(&gpuRegistryLock);
    if (gpuRegistered)
    {
        gpuOsInfo = registeredGpu.os_device_ptr;
    }
    ReleaseSRWLockShared(&gpuRegistryLock);

    switch (ops->op)
    {
        case NV01_FREE:
            NvRmExternalKernelFree(
                static_cast<NV_PLATFORM_RM_GPU_CONTEXT*>(gpuOsInfo),
                &ops->params.free);
            break;
        case NV01_ALLOC_MEMORY:
            NvRmExternalKernelAllocMemory(
                static_cast<NV_PLATFORM_RM_GPU_CONTEXT*>(gpuOsInfo),
                &ops->params.allocMemory64);
            break;
        case NV04_ALLOC:
            NvRmExternalKernelAlloc(
                static_cast<NV_PLATFORM_RM_GPU_CONTEXT*>(gpuOsInfo),
                &ops->params.alloc);
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
            if (ops->params.control.cmd == 0x20803D01U)
            {
                ops->params.control.status = NV_OK;
            }
            else if (ops->params.control.cmd == 0x00801E01U)
            {
                ops->params.control.status = NV_OK;
            }
            else if (ops->params.control.cmd == 0x00801E02U)
            {
                std::memset(NvP64_VALUE(ops->params.control.params), 0,
                            ops->params.control.paramsSize);
                ops->params.control.status = NV_OK;
            }
            else
            {
                NvRmExternalKernelControl(
                    static_cast<NV_PLATFORM_RM_GPU_CONTEXT*>(gpuOsInfo),
                    &ops->params.control);
            }
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

    const NvU32 operationStatus = ops->op == NV04_ALLOC
        ? ops->params.alloc.status
        : ops->op == NV04_CONTROL
            ? ops->params.control.status
            : ops->params.free.status;
    std::snprintf(traceMessage, sizeof(traceMessage),
                  "NVKMS RM operation end: op=0x%08X status=0x%08X",
                  ops->op, operationStatus);
    NvKmsWindowsTraceMessage(traceMessage);
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
    KmsDeviceLockGuard lock;

    NvKmsWindowsTraceMessage("NVKMS Windows initialize: module load begin");
    if (!nvKmsModuleLoad())
    {
        NvKmsWindowsTraceMessage("NVKMS Windows initialize: module load failed");
        return NV_ERR_GENERIC;
    }
    NvKmsWindowsTraceMessage("NVKMS Windows initialize: module load complete");

    kmsFunctions = {};
    kmsFunctions.versionString = "";
    if (nvKmsKapiGetFunctionsTableInternal(&kmsFunctions) ||
        kmsFunctions.versionString == nullptr ||
        !nvKmsKapiGetFunctionsTableInternal(&kmsFunctions))
    {
        nvKmsModuleUnload();
        return NV_ERR_LIB_RM_VERSION_MISMATCH;
    }

    enumeratedKmsGpu = {};
    kmsGpuEnumerated = NV_FALSE;
    kmsFunctions.enumerateGpus(NvKmsGpuCallback);
    if (!kmsGpuEnumerated)
    {
        NvKmsWindowsTraceMessage("NVKMS Windows initialize: GPU enumeration failed");
        nvKmsModuleUnload();
        return NV_ERR_OBJECT_NOT_FOUND;
    }
    NvKmsWindowsTraceMessage("NVKMS Windows initialize: GPU enumeration complete");

    NvKmsKapiAllocateDeviceParams allocateParams = {};
    allocateParams.gpuId = enumeratedKmsGpu.gpuInfo.gpu_id;
    allocateParams.migDevice = enumeratedKmsGpu.migDevice;
    kmsDevice = kmsFunctions.allocateDevice(&allocateParams);
    if (kmsDevice == nullptr)
    {
        NvKmsWindowsTraceMessage("NVKMS Windows initialize: device allocation failed");
        nvKmsModuleUnload();
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }
    NvKmsWindowsTraceMessage("NVKMS Windows initialize: device allocation complete");

    if (!kmsFunctions.grabOwnership(kmsDevice))
    {
        NvKmsWindowsTraceMessage("NVKMS Windows initialize: ownership acquisition failed");
        kmsFunctions.freeDevice(kmsDevice);
        kmsDevice = nullptr;
        nvKmsModuleUnload();
        return NV_ERR_STATE_IN_USE;
    }
    NvKmsWindowsTraceMessage("NVKMS Windows initialize: ownership acquisition complete");

    return NV_OK;
}

extern "C" void NvKmsWindowsShutdown(void)
{
    KmsDeviceLockGuard lock;

    if (kmsDevice != nullptr)
    {
        kmsFunctions.releaseOwnership(kmsDevice);
        kmsFunctions.freeDevice(kmsDevice);
        kmsDevice = nullptr;
    }
    nvKmsModuleUnload();
}

extern "C" NV_STATUS NvKmsWindowsProbeDisplays(
    NV_KMS_WINDOWS_DISPLAY_INFO* displayInfo)
{
    KmsDeviceLockGuard lock;

    if (displayInfo == nullptr || kmsDevice == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    *displayInfo = {};
    NvKmsKapiDeviceResourcesInfo resources = {};
    if (!kmsFunctions.getDeviceResourcesInfo(kmsDevice, &resources))
    {
        return NV_ERR_GENERIC;
    }
    displayInfo->connectorCount = resources.numConnectors;

    for (NvU32 index = 0; index < resources.numConnectors; ++index)
    {
        NvKmsKapiConnectorInfo connector = {};
        if (!kmsFunctions.getConnectorInfo(
                kmsDevice, resources.connectorHandles[index], &connector))
        {
            return NV_ERR_GENERIC;
        }

        char message[192] = {};
        nvkms_snprintf(
            message,
            sizeof(message),
            "connector[%u] handle=0x%08x physical=%u type=%u signal=%u dynamic=%u",
            index,
            connector.handle,
            connector.physicalIndex,
            connector.type,
            connector.signalFormat,
            connector.dynamicDpyIdListValid);
        nvkms_log(NVKMS_LOG_LEVEL_INFO, "", message);
    }

    NvU32 displayCount = 0;
    if (!kmsFunctions.getDisplays(kmsDevice, &displayCount, nullptr))
    {
        return NV_ERR_GENERIC;
    }
    displayInfo->displayCount = displayCount;
    if (displayCount == 0)
    {
        return NV_OK;
    }

    auto* const displays = static_cast<NvKmsKapiDisplay*>(
        nvkms_alloc(sizeof(NvKmsKapiDisplay) * displayCount, NV_TRUE));
    if (displays == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    NvU32 capacity = displayCount;
    if (!kmsFunctions.getDisplays(kmsDevice, &capacity, displays))
    {
        nvkms_free(displays, sizeof(NvKmsKapiDisplay) * displayCount);
        return NV_ERR_GENERIC;
    }
    displayInfo->displayCount = capacity;

    for (NvU32 index = 0; index < capacity; ++index)
    {
        NvKmsKapiStaticDisplayInfo staticInfo = {};
        NvKmsKapiDynamicDisplayParams dynamicInfo = {};
        dynamicInfo.handle = displays[index];
        if (!kmsFunctions.getStaticDisplayInfo(
                kmsDevice, displays[index], &staticInfo) ||
            !kmsFunctions.getDynamicDisplayInfo(kmsDevice, &dynamicInfo))
        {
            nvkms_free(displays, sizeof(NvKmsKapiDisplay) * displayCount);
            return NV_ERR_GENERIC;
        }

        if (dynamicInfo.connected)
        {
            ++displayInfo->connectedCount;
        }

        NvU8 checksum = 0;
        for (NvU32 byte = 0; byte < dynamicInfo.edid.bufferSize; ++byte)
        {
            checksum = static_cast<NvU8>(checksum + dynamicInfo.edid.buffer[byte]);
        }
        if (dynamicInfo.edid.bufferSize != 0)
        {
            displayInfo->edidBytes = dynamicInfo.edid.bufferSize;
            displayInfo->edidChecksum = checksum;
        }

        char message[256] = {};
        nvkms_snprintf(
            message,
            sizeof(message),
            "display[%u] handle=0x%08x connector=0x%08x connected=%u MST=%u EDID=%u checksum=0x%02x max=%ux%u",
            index,
            displays[index],
            staticInfo.connectorHandle,
            dynamicInfo.connected,
            staticInfo.isDpMST,
            dynamicInfo.edid.bufferSize,
            checksum,
            dynamicInfo.maxWidthInPixels,
            dynamicInfo.maxHeightInPixels);
        nvkms_log(NVKMS_LOG_LEVEL_INFO, "", message);

        for (NvU32 offset = 0; offset < dynamicInfo.edid.bufferSize; offset += 16)
        {
            char edidLine[96] = {};
            int used = nvkms_snprintf(
                edidLine, sizeof(edidLine), "EDID[%03x]:", offset);
            const NvU32 lineBytes =
                (dynamicInfo.edid.bufferSize - offset) < 16 ?
                (dynamicInfo.edid.bufferSize - offset) : 16;
            for (NvU32 byte = 0; byte < lineBytes && used > 0; ++byte)
            {
                used += nvkms_snprintf(
                    edidLine + used,
                    sizeof(edidLine) - static_cast<size_t>(used),
                    " %02x",
                    dynamicInfo.edid.buffer[offset + byte]);
            }
            nvkms_log(NVKMS_LOG_LEVEL_INFO, "", edidLine);
        }
    }

    nvkms_free(displays, sizeof(NvKmsKapiDisplay) * displayCount);
    return NV_OK;
}

extern "C" NV_STATUS NvKmsWindowsGetConnectedDisplayCount(
    NvU32* displayCount)
{
    if (displayCount == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NV_KMS_WINDOWS_DISPLAY_INFO displayInfo = {};
    const NV_STATUS status = NvKmsWindowsProbeDisplays(&displayInfo);
    if (status != NV_OK)
    {
        return status;
    }

    *displayCount = displayInfo.connectedCount;
    return NV_OK;
}

extern "C" NV_STATUS NvKmsWindowsGetDisplay(
    NvU32 connectedDisplayIndex,
    NV_KMS_WINDOWS_DISPLAY_STATE* displayState)
{
    if (displayState == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    KmsDeviceLockGuard lock;
    *displayState = {};
    displayState->preferredModeIndex = ~0U;
    if (kmsDevice == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    NvKmsKapiDeviceResourcesInfo resources = {};
    if (!kmsFunctions.getDeviceResourcesInfo(kmsDevice, &resources))
    {
        return NV_ERR_GENERIC;
    }

    displayState->headCount = resources.numHeads;
    for (NvU32 head = 0; head < resources.numHeads; ++head)
    {
        if (resources.numLayers[head] > displayState->maxLayersPerHead)
        {
            displayState->maxLayersPerHead = resources.numLayers[head];
        }
    }
    displayState->supportsMultiLayerFlip =
        displayState->maxLayersPerHead > 1 ? NV_TRUE : NV_FALSE;

    NvU32 displayCount = 0;
    if (!kmsFunctions.getDisplays(kmsDevice, &displayCount, nullptr))
    {
        return NV_ERR_GENERIC;
    }
    if (displayCount == 0)
    {
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    auto* const displays = static_cast<NvKmsKapiDisplay*>(
        nvkms_alloc(sizeof(NvKmsKapiDisplay) * displayCount, NV_TRUE));
    if (displays == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    NvU32 capacity = displayCount;
    NV_STATUS status = NV_ERR_OBJECT_NOT_FOUND;
    if (!kmsFunctions.getDisplays(kmsDevice, &capacity, displays))
    {
        status = NV_ERR_GENERIC;
        goto done;
    }

    NvU32 connectedIndex = 0;
    for (NvU32 displayIndex = 0; displayIndex < capacity; ++displayIndex)
    {
        NvKmsKapiStaticDisplayInfo staticInfo = {};
        NvKmsKapiDynamicDisplayParams dynamicInfo = {};
        dynamicInfo.handle = displays[displayIndex];
        if (!kmsFunctions.getStaticDisplayInfo(
                kmsDevice, displays[displayIndex], &staticInfo) ||
            !kmsFunctions.getDynamicDisplayInfo(kmsDevice, &dynamicInfo))
        {
            status = NV_ERR_GENERIC;
            goto done;
        }
        if (!dynamicInfo.connected)
        {
            continue;
        }
        if (connectedIndex++ != connectedDisplayIndex)
        {
            continue;
        }

        displayState->displayHandle = displays[displayIndex];

        for (NvU32 connectorIndex = 0;
             connectorIndex < resources.numConnectors;
             ++connectorIndex)
        {
            NvKmsKapiConnectorInfo connector = {};
            if (!kmsFunctions.getConnectorInfo(
                    kmsDevice,
                    resources.connectorHandles[connectorIndex],
                    &connector))
            {
                status = NV_ERR_GENERIC;
                goto done;
            }
            if (connector.handle == staticInfo.connectorHandle)
            {
                displayState->connectorType = connector.type;
                displayState->connectorIndex = connector.physicalIndex;
                break;
            }
        }

        displayState->edidBytes = dynamicInfo.edid.bufferSize;
        if (displayState->edidBytes > NV_KMS_WINDOWS_MAX_EDID_BYTES)
        {
            displayState->edidBytes = NV_KMS_WINDOWS_MAX_EDID_BYTES;
        }
        std::memcpy(
            displayState->edid,
            dynamicInfo.edid.buffer,
            displayState->edidBytes);

        for (NvU32 modeIndex = 0;
             displayState->modeCount < NV_KMS_WINDOWS_MAX_DISPLAY_MODES;
             ++modeIndex)
        {
            NvKmsKapiDisplayMode kmsMode = {};
            NvBool valid = NV_FALSE;
            NvBool preferred = NV_FALSE;
            const int moreModes = kmsFunctions.getDisplayMode(
                kmsDevice,
                displays[displayIndex],
                modeIndex,
                &kmsMode,
                &valid,
                &preferred);
            if (moreModes < 0)
            {
                status = NV_ERR_GENERIC;
                goto done;
            }

            if (valid)
            {
                NV_KMS_WINDOWS_MODE* const mode =
                    &displayState->modes[displayState->modeCount];
                mode->kmsModeIndex = modeIndex;
                mode->refreshRateMillihertz = kmsMode.timings.refreshRate;
                mode->pixelClockHz = kmsMode.timings.pixelClockHz;
                mode->hVisible = kmsMode.timings.hVisible;
                mode->hSyncStart = kmsMode.timings.hSyncStart;
                mode->hSyncEnd = kmsMode.timings.hSyncEnd;
                mode->hTotal = kmsMode.timings.hTotal;
                mode->vVisible = kmsMode.timings.vVisible;
                mode->vSyncStart = kmsMode.timings.vSyncStart;
                mode->vSyncEnd = kmsMode.timings.vSyncEnd;
                mode->vTotal = kmsMode.timings.vTotal;
                mode->widthMillimeters = kmsMode.timings.widthMM;
                mode->heightMillimeters = kmsMode.timings.heightMM;
                mode->interlaced = kmsMode.timings.flags.interlaced;
                mode->preferred = preferred;
                if (preferred)
                {
                    displayState->preferredModeIndex = displayState->modeCount;
                }
                ++displayState->modeCount;
            }

            if (moreModes == 0)
            {
                break;
            }
        }

        status = displayState->modeCount == 0 ?
            NV_ERR_OBJECT_NOT_FOUND : NV_OK;
        break;
    }

done:
    nvkms_free(displays, sizeof(NvKmsKapiDisplay) * displayCount);
    return status;
}

extern "C" NV_STATUS NvKmsWindowsGetPrimaryDisplay(
    NV_KMS_WINDOWS_DISPLAY_STATE* displayState)
{
    return NvKmsWindowsGetDisplay(0, displayState);
}

extern "C" NV_STATUS NvKmsWindowsCreateScanoutSurface(
    NvU32 width,
    NvU32 height,
    NV_KMS_WINDOWS_SURFACE** surface,
    void** pixels,
    NvU32* pitch)
{
    if (width == 0 || height == 0 || surface == nullptr ||
        pixels == nullptr || pitch == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *surface = nullptr;
    *pixels = nullptr;
    *pitch = 0;

    KmsDeviceLockGuard lock;
    if (kmsDevice == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    NvKmsKapiDeviceResourcesInfo resources = {};
    if (!kmsFunctions.getDeviceResourcesInfo(kmsDevice, &resources) ||
        resources.numHeads == 0 || resources.numLayers[0] == 0)
    {
        return NV_ERR_GENERIC;
    }
    if ((resources.supportedSurfaceMemoryFormats[0] &
         NVBIT64(NvKmsSurfaceMemoryFormatA8R8G8B8)) == 0)
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    const NvU32 alignment = resources.caps.pitchAlignment == 0 ?
        1 : resources.caps.pitchAlignment;
    const NvU64 rowBytes = static_cast<NvU64>(width) * 4;
    const NvU64 alignedPitch =
        ((rowBytes + alignment - 1) / alignment) * alignment;
    const NvU64 allocationSize = alignedPitch * height;
    if (alignedPitch > ~static_cast<NvU32>(0) ||
        allocationSize / height != alignedPitch)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    auto* const scanout = static_cast<NV_KMS_WINDOWS_SURFACE*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                  sizeof(NV_KMS_WINDOWS_SURFACE)));
    if (scanout == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    NvU8 compressible = 0;
    NvKmsKapiAllocateMemoryParams memoryParams = {};
    memoryParams.layout = NvKmsSurfaceMemoryLayoutPitch;
    memoryParams.type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT;
    memoryParams.size = allocationSize;
    memoryParams.noDisplayCaching = NV_TRUE;
    memoryParams.useVideoMemory = NV_FALSE;
    memoryParams.compressible = &compressible;
    scanout->memory = kmsFunctions.allocateMemory(kmsDevice, &memoryParams);
    if (scanout->memory == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, scanout);
        return NV_ERR_NO_MEMORY;
    }

    if (!kmsFunctions.mapMemory(
            kmsDevice, scanout->memory, NVKMS_KAPI_MAPPING_TYPE_USER,
            &scanout->pixels))
    {
        kmsFunctions.freeMemory(kmsDevice, scanout->memory);
        HeapFree(GetProcessHeap(), 0, scanout);
        return NV_ERR_GENERIC;
    }

    scanout->size = allocationSize;
    scanout->width = width;
    scanout->height = height;
    scanout->pitch = static_cast<NvU32>(alignedPitch);

    NvKmsKapiCreateSurfaceParams surfaceParams = {};
    surfaceParams.planes[0].memory = scanout->memory;
    surfaceParams.planes[0].pitch = scanout->pitch;
    surfaceParams.width = width;
    surfaceParams.height = height;
    surfaceParams.format = NvKmsSurfaceMemoryFormatA8R8G8B8;
    surfaceParams.explicit_layout = NV_TRUE;
    surfaceParams.layout = NvKmsSurfaceMemoryLayoutPitch;
    scanout->surface = kmsFunctions.createSurface(kmsDevice, &surfaceParams);
    if (scanout->surface == nullptr)
    {
        kmsFunctions.unmapMemory(
            kmsDevice, scanout->memory, NVKMS_KAPI_MAPPING_TYPE_USER,
            scanout->pixels);
        kmsFunctions.freeMemory(kmsDevice, scanout->memory);
        HeapFree(GetProcessHeap(), 0, scanout);
        return NV_ERR_GENERIC;
    }

    *surface = scanout;
    *pixels = scanout->pixels;
    *pitch = scanout->pitch;
    return NV_OK;
}

extern "C" void NvKmsWindowsDestroyScanoutSurface(
    NV_KMS_WINDOWS_SURFACE* surface)
{
    if (surface == nullptr)
    {
        return;
    }

    KmsDeviceLockGuard lock;
    if (kmsDevice != nullptr)
    {
        if (surface->surface != nullptr)
        {
            kmsFunctions.destroySurface(kmsDevice, surface->surface);
        }
        if (surface->pixels != nullptr && surface->memory != nullptr)
        {
            kmsFunctions.unmapMemory(
                kmsDevice, surface->memory, NVKMS_KAPI_MAPPING_TYPE_USER,
                surface->pixels);
        }
        if (surface->memory != nullptr)
        {
            kmsFunctions.freeMemory(kmsDevice, surface->memory);
        }
    }
    HeapFree(GetProcessHeap(), 0, surface);
}

extern "C" NV_STATUS NvKmsWindowsSetDisplayModes(
    NV_KMS_WINDOWS_MODESET_OUTPUT* outputs,
    NvU32 outputCount)
{
    if ((outputs == nullptr && outputCount != 0) ||
        outputCount > NVKMS_KAPI_MAX_HEADS)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    KmsDeviceLockGuard lock;
    if (kmsDevice == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    NvKmsKapiDeviceResourcesInfo resources = {};
    if (!kmsFunctions.getDeviceResourcesInfo(kmsDevice, &resources) ||
        outputCount > resources.numHeads)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NvKmsKapiDisplayMode modes[NVKMS_KAPI_MAX_HEADS] = {};
    for (NvU32 index = 0; index < outputCount; ++index)
    {
        if (outputs[index].surface == nullptr)
        {
            return NV_ERR_INVALID_ARGUMENT;
        }

        NvBool valid = NV_FALSE;
        NvBool preferred = NV_FALSE;
        if (kmsFunctions.getDisplayMode(
                kmsDevice,
                outputs[index].displayHandle,
                outputs[index].modeIndex,
                &modes[index],
                &valid,
                &preferred) < 0 ||
            !valid ||
            outputs[index].surface->width != modes[index].timings.hVisible ||
            outputs[index].surface->height != modes[index].timings.vVisible)
        {
            return NV_ERR_INVALID_ARGUMENT;
        }

        NvKmsKapiDynamicDisplayParams dynamicInfo = {};
        dynamicInfo.handle = outputs[index].displayHandle;
        if (!kmsFunctions.getDynamicDisplayInfo(kmsDevice, &dynamicInfo) ||
            !dynamicInfo.connected)
        {
            return NV_ERR_OBJECT_NOT_FOUND;
        }
    }

    NvU32 assignmentCount = 1;
    for (NvU32 index = 0; index < outputCount; ++index)
    {
        assignmentCount *= resources.numHeads;
    }

    for (NvU32 encodedAssignment = 0;
         encodedAssignment < assignmentCount;
         ++encodedAssignment)
    {
        NvU32 assignedHeads[NVKMS_KAPI_MAX_HEADS] = {};
        NvU32 remaining = encodedAssignment;
        NvU32 usedHeads = 0;
        bool assignmentValid = true;
        for (NvU32 index = 0; index < outputCount; ++index)
        {
            const NvU32 head = remaining % resources.numHeads;
            remaining /= resources.numHeads;
            if ((usedHeads & (1U << head)) != 0 ||
                resources.numLayers[head] == 0)
            {
                assignmentValid = false;
                break;
            }
            usedHeads |= 1U << head;
            assignedHeads[index] = head;
        }
        if (!assignmentValid)
        {
            continue;
        }

        NvKmsKapiRequestedModeSetConfig requested = {};
        NvKmsKapiModeSetReplyConfig reply = {};
        for (NvU32 head = 0; head < resources.numHeads; ++head)
        {
            requested.headsMask |= 1U << head;
            auto* const headConfig = &requested.headRequestedConfig[head];
            headConfig->flags.activeChanged = NV_TRUE;
            headConfig->flags.displaysChanged = NV_TRUE;
            for (NvU32 layer = 0; layer < resources.numLayers[head]; ++layer)
            {
                headConfig->layerRequestedConfig[layer]
                    .flags.surfaceChanged = NV_TRUE;
            }
        }

        for (NvU32 index = 0; index < outputCount; ++index)
        {
            const NvU32 head = assignedHeads[index];
            auto* const headConfig = &requested.headRequestedConfig[head];
            headConfig->modeSetConfig.bActive = NV_TRUE;
            headConfig->modeSetConfig.numDisplays = 1;
            headConfig->modeSetConfig.displays[0] = outputs[index].displayHandle;
            headConfig->modeSetConfig.mode = modes[index];
            headConfig->flags.modeChanged = NV_TRUE;

            auto* const layer =
                &headConfig->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
            layer->config.surface = outputs[index].surface->surface;
            layer->config.srcWidth =
                static_cast<NvU16>(modes[index].timings.hVisible);
            layer->config.srcHeight =
                static_cast<NvU16>(modes[index].timings.vVisible);
            layer->config.dstWidth = layer->config.srcWidth;
            layer->config.dstHeight = layer->config.srcHeight;
            layer->config.compParams.compMode =
                NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
            layer->config.compParams.surfaceAlpha = 255;
            layer->config.minPresentInterval = 1;
            layer->flags.srcXYChanged = NV_TRUE;
            layer->flags.srcWHChanged = NV_TRUE;
            layer->flags.dstXYChanged = NV_TRUE;
            layer->flags.dstWHChanged = NV_TRUE;
        }

        if (!kmsFunctions.applyModeSetConfig(
                kmsDevice, &requested, &reply, NV_FALSE))
        {
            continue;
        }

        FlushProcessWriteBuffers();
        reply = {};
        if (!kmsFunctions.applyModeSetConfig(
                kmsDevice, &requested, &reply, NV_TRUE))
        {
            return reply.flipResult == NV_KMS_FLIP_RESULT_IN_PROGRESS ?
                NV_ERR_IN_USE : NV_ERR_INVALID_STATE;
        }

        for (NvU32 index = 0; index < outputCount; ++index)
        {
            outputs[index].assignedHead = assignedHeads[index];
        }
        return NV_OK;
    }

    return NV_ERR_NOT_SUPPORTED;
}

extern "C" NV_STATUS NvKmsWindowsPresentLayers(
    NvU32 head,
    const NV_KMS_WINDOWS_LAYER* layers,
    NvU32 layerCount)
{
    if ((layers == nullptr && layerCount != 0) ||
        layerCount > NV_KMS_WINDOWS_MAX_LAYERS)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    KmsDeviceLockGuard lock;
    if (kmsDevice == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }

    NvKmsKapiDeviceResourcesInfo resources = {};
    if (!kmsFunctions.getDeviceResourcesInfo(kmsDevice, &resources) ||
        head >= resources.numHeads || layerCount > resources.numLayers[head])
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    NvKmsKapiRequestedModeSetConfig requested = {};
    NvKmsKapiModeSetReplyConfig reply = {};
    requested.headsMask = 1U << head;
    auto* const headConfig = &requested.headRequestedConfig[head];

    for (NvU32 layerIndex = 0;
         layerIndex < resources.numLayers[head];
         ++layerIndex)
    {
        auto* const requestedLayer =
            &headConfig->layerRequestedConfig[layerIndex];
        requestedLayer->flags.surfaceChanged = NV_TRUE;
        if (layerIndex >= layerCount)
        {
            continue;
        }

        const NV_KMS_WINDOWS_LAYER* const layer = &layers[layerIndex];
        if (layer->surface == nullptr || layer->sourceWidth == 0 ||
            layer->sourceHeight == 0 || layer->destinationWidth == 0 ||
            layer->destinationHeight == 0 ||
            layer->sourceX > 0xffffU || layer->sourceY > 0xffffU ||
            layer->sourceWidth > 0xffffU ||
            layer->sourceHeight > 0xffffU ||
            layer->destinationX < -32768 ||
            layer->destinationX > 32767 ||
            layer->destinationY < -32768 ||
            layer->destinationY > 32767 ||
            layer->destinationWidth > 0xffffU ||
            layer->destinationHeight > 0xffffU ||
            layer->sourceX + layer->sourceWidth > layer->surface->width ||
            layer->sourceY + layer->sourceHeight > layer->surface->height)
        {
            return NV_ERR_INVALID_ARGUMENT;
        }

        auto* const config = &requestedLayer->config;
        config->surface = layer->surface->surface;
        config->srcX = static_cast<NvU16>(layer->sourceX);
        config->srcY = static_cast<NvU16>(layer->sourceY);
        config->srcWidth = static_cast<NvU16>(layer->sourceWidth);
        config->srcHeight = static_cast<NvU16>(layer->sourceHeight);
        config->dstX = static_cast<NvS16>(layer->destinationX);
        config->dstY = static_cast<NvS16>(layer->destinationY);
        config->dstWidth = static_cast<NvU16>(layer->destinationWidth);
        config->dstHeight = static_cast<NvU16>(layer->destinationHeight);
        config->compParams.compMode = layer->opacity == 255 ?
            NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE :
            NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_SURFACE_ALPHA;
        config->compParams.surfaceAlpha = layer->opacity;
        config->minPresentInterval = 1;

        requestedLayer->flags.srcXYChanged = NV_TRUE;
        requestedLayer->flags.srcWHChanged = NV_TRUE;
        requestedLayer->flags.dstXYChanged = NV_TRUE;
        requestedLayer->flags.dstWHChanged = NV_TRUE;
    }

    if (!kmsFunctions.applyModeSetConfig(
            kmsDevice, &requested, &reply, NV_TRUE))
    {
        return reply.flipResult == NV_KMS_FLIP_RESULT_IN_PROGRESS ?
            NV_ERR_IN_USE : NV_ERR_INVALID_STATE;
    }
    return NV_OK;
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
