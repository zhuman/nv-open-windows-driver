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

namespace {

SRWLOCK gpuRegistryLock = SRWLOCK_INIT;
nv_gpu_info_t registeredGpu = {};
NvBool gpuRegistered = NV_FALSE;
NvKmsKapiFunctionsTable kmsFunctions = {};
NvKmsKapiDevice* kmsDevice = nullptr;
NvKmsKapiGpuInfo enumeratedKmsGpu = {};
NvBool kmsGpuEnumerated = NV_FALSE;
INIT_ONCE kmsLockInitOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION kmsLock;

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
        NvKmsLock();
        timer->proc(dataPtr, timer->dataU32);
        NvKmsUnlock();
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
    WaitForThreadpoolTimerCallbacks(timer->timer, TRUE);
    CloseThreadpoolTimer(timer->timer);
    HeapFree(GetProcessHeap(), 0, timer);
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
    if (!nvKmsModuleLoad())
    {
        return NV_ERR_GENERIC;
    }

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
        nvKmsModuleUnload();
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    NvKmsKapiAllocateDeviceParams allocateParams = {};
    allocateParams.gpuId = enumeratedKmsGpu.gpuInfo.gpu_id;
    allocateParams.migDevice = enumeratedKmsGpu.migDevice;
    kmsDevice = kmsFunctions.allocateDevice(&allocateParams);
    if (kmsDevice == nullptr)
    {
        nvKmsModuleUnload();
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    return NV_OK;
}

extern "C" void NvKmsWindowsShutdown(void)
{
    if (kmsDevice != nullptr)
    {
        kmsFunctions.freeDevice(kmsDevice);
        kmsDevice = nullptr;
    }
    nvKmsModuleUnload();
}

extern "C" NV_STATUS NvKmsWindowsProbeDisplays(
    NV_KMS_WINDOWS_DISPLAY_INFO* displayInfo)
{
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
