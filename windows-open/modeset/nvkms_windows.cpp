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
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <windows.h>

#ifndef NV_KMS_WINDOWS_VERIFY_SCANOUT_UPLOAD
#define NV_KMS_WINDOWS_VERIFY_SCANOUT_UPLOAD 0
#endif

#ifndef NV_KMS_WINDOWS_BYPASS_COMPOSITION
#define NV_KMS_WINDOWS_BYPASS_COMPOSITION 0
#endif

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
    NvKmsKapiMemory* stagingMemory;
    NvKmsKapiSurface* surface;
    void* pixels;
    NvU64 size;
    NvU32 width;
    NvU32 height;
    NvU32 pitch;
    NV_KMS_WINDOWS_SCANOUT_FORMAT format;
    NvBool presented;
};

extern "C" void NvKmsWindowsTraceMessage(const char* message);
extern "C" NvBool nvEvoGetPqOutputLutC5(
    void* data, NvU32 size, NvU32* lutEntries, NvU32* vssSegments);

namespace {

SRWLOCK gpuRegistryLock = SRWLOCK_INIT;
nv_gpu_info_t registeredGpu = {};
NvBool gpuRegistered = NV_FALSE;
NvKmsKapiFunctionsTable kmsFunctions = {};
NvKmsKapiDevice* kmsDevice = nullptr;
NV_KMS_WINDOWS_SURFACE* kmsDefaultLutSurface = nullptr;
NV_KMS_WINDOWS_SURFACE* kmsGammaSurfaces[NVKMS_KAPI_MAX_HEADS] = {};
NvKmsKapiGpuInfo enumeratedKmsGpu = {};
NvBool kmsGpuEnumerated = NV_FALSE;
std::atomic<NV_KMS_WINDOWS_DISPLAY_CHANGE_CALLBACK>
    displayChangeCallback{nullptr};
std::atomic<void*> displayChangeContext{nullptr};

constexpr NvU32 LutHeaderEntries = 4;
constexpr NvU32 LutEntries = 1025;
constexpr NvU32 LutSurfaceAlignment = 0x100;
NvU32 pqLutEntries = 0;
NvU32 pqLutSegments = 0;

struct HeadGammaCache
{
    NvU16 red[NV_KMS_WINDOWS_GAMMA_ENTRIES];
    NvU16 green[NV_KMS_WINDOWS_GAMMA_ENTRIES];
    NvU16 blue[NV_KMS_WINDOWS_GAMMA_ENTRIES];
    NvBool valid;
};

HeadGammaCache gammaCache[NVKMS_KAPI_MAX_HEADS] = {};
NvKmsKapiLayerConfig
    layerCache[NVKMS_KAPI_MAX_HEADS][NVKMS_KAPI_LAYER_MAX] = {};

struct LutEntry
{
    NvU16 Red;
    NvU16 Green;
    NvU16 Blue;
    NvU16 Reserved;
};

struct LutData
{
    LutEntry base[LutHeaderEntries + LutEntries];
    alignas(LutSurfaceAlignment)
        LutEntry output[LutHeaderEntries + LutEntries];
    alignas(LutSurfaceAlignment)
        LutEntry pqOutput[LutHeaderEntries + LutEntries];
};

NvU16 FloatToFp16(float value)
{
    if (value == 0)
    {
        return 0;
    }

    NvU32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    NvU32 exponent = (bits >> 23) & 0xFF;
    NvU32 mantissa = bits & 0x7FFFFF;
    mantissa += 0xFFF + ((mantissa >> 13) & 1);
    if ((mantissa & 0x800000) != 0)
    {
        mantissa = 0;
        ++exponent;
    }

    return static_cast<NvU16>(((exponent - 112) << 10) |
                              (mantissa >> 13));
}

NvU16 Unorm10ToFp16(NvU16 value)
{
    return FloatToFp16(static_cast<float>(value) / 1023.0F);
}

void CopyHdrMetadata(
    const NV_KMS_WINDOWS_HDR_METADATA& source,
    NvKmsHDRStaticMetadata* destination)
{
    for (NvU32 primary = 0; primary < 3; ++primary)
    {
        destination->displayPrimaries[primary].x =
            source.displayPrimaries[primary][0];
        destination->displayPrimaries[primary].y =
            source.displayPrimaries[primary][1];
    }
    destination->whitePoint.x = source.whitePoint[0];
    destination->whitePoint.y = source.whitePoint[1];
    destination->maxDisplayMasteringLuminance =
        source.maxMasteringLuminance;
    destination->minDisplayMasteringLuminance =
        source.minMasteringLuminance;
    destination->maxCLL = source.maxContentLightLevel;
    destination->maxFALL = source.maxFrameAverageLightLevel;
}
SRWLOCK kmsDeviceLock = SRWLOCK_INIT;
INIT_ONCE kmsLockInitOnce = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION kmsLock;

#if NV_KMS_WINDOWS_VERIFY_SCANOUT_UPLOAD
NvU32 HashSurface(const void* pixels, NvU64 size)
{
    const auto* const bytes = static_cast<const NvU8*>(pixels);
    NvU32 hash = 2166136261U;
    for (NvU64 index = 0; index < size; ++index)
    {
        hash = (hash ^ bytes[index]) * 16777619U;
    }
    return hash;
}
#endif

class KmsDeviceLockGuard
{
public:
    KmsDeviceLockGuard() { AcquireSRWLockExclusive(&kmsDeviceLock); }
    ~KmsDeviceLockGuard() { ReleaseSRWLockExclusive(&kmsDeviceLock); }

    KmsDeviceLockGuard(const KmsDeviceLockGuard&) = delete;
    KmsDeviceLockGuard& operator=(const KmsDeviceLockGuard&) = delete;
};

NvU64 PerformanceCounter()
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<NvU64>(counter.QuadPart);
}

void DestroySurface(NV_KMS_WINDOWS_SURFACE* surface)
{
    if (surface == nullptr)
    {
        return;
    }

    if (kmsDevice != nullptr)
    {
        if (surface->surface != nullptr)
        {
            kmsFunctions.destroySurface(kmsDevice, surface->surface);
        }
        if (surface->pixels != nullptr && surface->stagingMemory != nullptr)
        {
            kmsFunctions.unmapMemory(
                kmsDevice, surface->stagingMemory,
                NVKMS_KAPI_MAPPING_TYPE_USER,
                surface->pixels);
        }
        if (surface->stagingMemory != nullptr)
        {
            kmsFunctions.freeMemory(kmsDevice, surface->stagingMemory);
        }
        if (surface->memory != nullptr)
        {
            kmsFunctions.freeMemory(kmsDevice, surface->memory);
        }
    }
    HeapFree(GetProcessHeap(), 0, surface);
}

NvU16 SampleGamma(const NvU16* ramp, NvU16 value)
{
    const NvU32 position = static_cast<NvU32>(value) *
        (NV_KMS_WINDOWS_GAMMA_ENTRIES - 1);
    const NvU32 lower = position / 65535;
    const NvU32 upper = lower + 1 < NV_KMS_WINDOWS_GAMMA_ENTRIES ?
        lower + 1 : lower;
    const NvU32 fraction = position % 65535;
    return static_cast<NvU16>(
        (static_cast<NvU32>(ramp[lower]) * (65535 - fraction) +
         static_cast<NvU32>(ramp[upper]) * fraction + 32767) / 65535);
}

NV_STATUS CreateLutSurface(
    const NV_KMS_WINDOWS_COLOR_TRANSFORM* transform,
    NV_KMS_WINDOWS_SURFACE** result)
{
    const NvU64 size =
        (sizeof(LutData) + LutSurfaceAlignment - 1) &
        ~(static_cast<NvU64>(LutSurfaceAlignment) - 1);
    auto* const lut = static_cast<NV_KMS_WINDOWS_SURFACE*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                  sizeof(NV_KMS_WINDOWS_SURFACE)));
    if (lut == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    NvU8 compressible = 0;
    NvKmsKapiAllocateMemoryParams memoryParams = {};
    memoryParams.layout = NvKmsSurfaceMemoryLayoutPitch;
    memoryParams.type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT;
    memoryParams.size = size;
    memoryParams.noDisplayCaching = NV_TRUE;
    memoryParams.useVideoMemory = NV_TRUE;
    memoryParams.compressible = &compressible;
    lut->memory = kmsFunctions.allocateMemory(kmsDevice, &memoryParams);
    if (lut->memory == nullptr)
    {
        DestroySurface(lut);
        return NV_ERR_NO_MEMORY;
    }

    memoryParams.useVideoMemory = NV_FALSE;
    lut->stagingMemory = kmsFunctions.allocateMemory(kmsDevice, &memoryParams);
    if (lut->stagingMemory == nullptr ||
        !kmsFunctions.mapMemory(
            kmsDevice, lut->stagingMemory, NVKMS_KAPI_MAPPING_TYPE_USER,
            &lut->pixels))
    {
        DestroySurface(lut);
        return NV_ERR_NO_MEMORY;
    }

    lut->size = size;
    lut->width = static_cast<NvU32>(size / sizeof(LutEntry));
    lut->height = 1;
    lut->pitch = static_cast<NvU32>(size);

    auto* const data = static_cast<LutData*>(lut->pixels);
    std::memset(data, 0, static_cast<size_t>(size));
    for (NvU32 index = 0; index < 1024; ++index)
    {
        data->base[LutHeaderEntries + index].Red =
        data->base[LutHeaderEntries + index].Green =
        data->base[LutHeaderEntries + index].Blue =
            Unorm10ToFp16(static_cast<NvU16>(index));
        data->output[LutHeaderEntries + index].Red =
        data->output[LutHeaderEntries + index].Green =
        data->output[LutHeaderEntries + index].Blue =
            static_cast<NvU16>(index << 6);
    }
    data->base[LutHeaderEntries + 1024] =
        data->base[LutHeaderEntries + 1023];
    data->output[LutHeaderEntries + 1024] =
        data->output[LutHeaderEntries + 1023];
    if (!nvEvoGetPqOutputLutC5(
            data->pqOutput, sizeof(data->pqOutput),
            &pqLutEntries, &pqLutSegments))
    {
        DestroySurface(lut);
        return NV_ERR_GENERIC;
    }
    if (transform != nullptr && transform->gammaEnabled)
    {
        for (NvU32 index = LutHeaderEntries;
             index < LutHeaderEntries + LutEntries;
             ++index)
        {
            auto& entry = data->output[index];
            entry.Red = SampleGamma(transform->gammaRed, entry.Red);
            entry.Green = SampleGamma(transform->gammaGreen, entry.Green);
            entry.Blue = SampleGamma(transform->gammaBlue, entry.Blue);
        }
        for (NvU32 index = LutHeaderEntries;
             index < LutHeaderEntries + pqLutEntries;
             ++index)
        {
            auto& entry = data->pqOutput[index];
            entry.Red = SampleGamma(transform->gammaRed, entry.Red);
            entry.Green = SampleGamma(transform->gammaGreen, entry.Green);
            entry.Blue = SampleGamma(transform->gammaBlue, entry.Blue);
        }
    }

    NvKmsKapiCreateSurfaceParams surfaceParams = {};
    surfaceParams.planes[0].memory = lut->memory;
    surfaceParams.planes[0].pitch = lut->pitch;
    surfaceParams.width = lut->width;
    surfaceParams.height = lut->height;
    surfaceParams.format = NvKmsSurfaceMemoryFormatR16G16B16A16;
    surfaceParams.explicit_layout = NV_TRUE;
    surfaceParams.layout = NvKmsSurfaceMemoryLayoutPitch;
    lut->surface = kmsFunctions.createSurface(kmsDevice, &surfaceParams);
    FlushProcessWriteBuffers();
    if (lut->surface == nullptr ||
        !kmsFunctions.copyMemory(
            kmsDevice, lut->memory, lut->stagingMemory, lut->size))
    {
        DestroySurface(lut);
        return NV_ERR_GENERIC;
    }

    *result = lut;
    NvKmsWindowsTraceMessage("NVKMS LUT uploaded to vidmem");
    return NV_OK;
}

NV_STATUS EnsureDefaultLutSurface()
{
    return kmsDefaultLutSurface != nullptr ? NV_OK :
        CreateLutSurface(nullptr, &kmsDefaultLutSurface);
}

void SetOutputLut(NvKmsKapiHeadRequestedConfig* headConfig,
                  NV_KMS_WINDOWS_SURFACE* surface, NvBool hdr)
{
    headConfig->modeSetConfig.olut.enabled = NV_TRUE;
    headConfig->modeSetConfig.olut.lutSurface = surface->surface;
    headConfig->modeSetConfig.olut.offset = hdr ?
        offsetof(LutData, pqOutput) : offsetof(LutData, output);
    headConfig->modeSetConfig.olut.vssSegments = hdr ? pqLutSegments : 0;
    headConfig->modeSetConfig.olut.lutEntries = hdr ? pqLutEntries : LutEntries;
    headConfig->modeSetConfig.olutFpNormScale = hdr ?
        NVKMS_OLUT_FP_NORM_SCALE_DEFAULT / 125 :
        NVKMS_OLUT_FP_NORM_SCALE_DEFAULT;
    headConfig->flags.olutChanged = NV_TRUE;
    headConfig->flags.olutFpNormScaleChanged = NV_TRUE;
}

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

void NvKmsEventCallback(const NvKmsKapiEvent* event)
{
    if (event == nullptr)
    {
        return;
    }

    NvKmsKapiDisplay display = 0;
    if (event->type == NVKMS_EVENT_TYPE_DPY_CHANGED)
    {
        display = event->u.displayChanged.display;
    }
    else if (event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED)
    {
        display = event->u.dynamicDisplayConnected.display;
    }
    else if (event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED)
    {
        display = event->u.dynamicDisplayDisconnected.display;
    }

    char traceMessage[112] = {};
    std::snprintf(
        traceMessage,
        sizeof(traceMessage),
        "NVKMS event: type=%u display=0x%08X head=%u layer=%u",
        static_cast<NvU32>(event->type),
        display,
        event->type == NVKMS_EVENT_TYPE_FLIP_OCCURRED ?
            event->u.flipOccurred.head : 0,
        event->type == NVKMS_EVENT_TYPE_FLIP_OCCURRED ?
            event->u.flipOccurred.layer : 0);
    NvKmsWindowsTraceMessage(traceMessage);

    if (event->type == NVKMS_EVENT_TYPE_DPY_CHANGED ||
        event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED ||
        event->type == NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED)
    {
        const auto callback = displayChangeCallback.load(std::memory_order_acquire);
        if (callback != nullptr)
        {
            callback(displayChangeContext.load(std::memory_order_relaxed));
        }
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

extern "C" void NvKmsWindowsSetDisplayChangeCallback(
    NV_KMS_WINDOWS_DISPLAY_CHANGE_CALLBACK callback,
    void* context)
{
    if (callback == nullptr)
    {
        displayChangeCallback.store(nullptr, std::memory_order_release);
        displayChangeContext.store(nullptr, std::memory_order_relaxed);
        return;
    }

    displayChangeContext.store(context, std::memory_order_relaxed);
    displayChangeCallback.store(callback, std::memory_order_release);
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
    allocateParams.eventCallback = NvKmsEventCallback;
    kmsDevice = kmsFunctions.allocateDevice(&allocateParams);
    if (kmsDevice == nullptr)
    {
        NvKmsWindowsTraceMessage("NVKMS Windows initialize: device allocation failed");
        nvKmsModuleUnload();
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }
    NvKmsWindowsTraceMessage("NVKMS Windows initialize: device allocation complete");

    const NvU32 displayEventInterest =
        (1U << NVKMS_EVENT_TYPE_DPY_CHANGED) |
        (1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED) |
        (1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED);
    if (!kmsFunctions.declareEventInterest(kmsDevice, displayEventInterest))
    {
        NvKmsWindowsTraceMessage("NVKMS Windows initialize: event registration failed");
        kmsFunctions.freeDevice(kmsDevice);
        kmsDevice = nullptr;
        nvKmsModuleUnload();
        return NV_ERR_GENERIC;
    }
    NvKmsWindowsTraceMessage("NVKMS Windows initialize: event registration complete");

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
        for (auto*& surface : kmsGammaSurfaces)
        {
            DestroySurface(surface);
            surface = nullptr;
        }
        DestroySurface(kmsDefaultLutSurface);
        kmsDefaultLutSurface = nullptr;
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
    if ((resources.caps.supportedCursorSurfaceMemoryFormats &
         NVBIT64(NvKmsSurfaceMemoryFormatA8R8G8B8)) != 0 &&
        (resources.caps.validCursorCompositionModes &
         NVBIT(NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA)) != 0)
    {
        displayState->maxCursorSize =
            resources.caps.maxCursorSizeInPixels > 512 ?
                512 : resources.caps.maxCursorSizeInPixels;
    }
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

            const NvBool rgb8Supported = valid ?
                kmsFunctions.validateDisplayModeColor(
                    kmsDevice,
                    displays[displayIndex],
                    &kmsMode,
                    NVKMS_KAPI_REQUESTED_COLOR_FORMAT_RGB,
                    8) : NV_FALSE;
            if (rgb8Supported)
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
                mode->hSkew = kmsMode.timings.hSkew;
                mode->vVisible = kmsMode.timings.vVisible;
                mode->vSyncStart = kmsMode.timings.vSyncStart;
                mode->vSyncEnd = kmsMode.timings.vSyncEnd;
                mode->vTotal = kmsMode.timings.vTotal;
                mode->widthMillimeters = kmsMode.timings.widthMM;
                mode->heightMillimeters = kmsMode.timings.heightMM;
                mode->rgb8Supported = NV_TRUE;
                mode->rgb10Supported =
                    kmsFunctions.validateDisplayModeColor(
                        kmsDevice,
                        displays[displayIndex],
                        &kmsMode,
                        NVKMS_KAPI_REQUESTED_COLOR_FORMAT_RGB,
                        10);
                mode->interlaced = kmsMode.timings.flags.interlaced;
                mode->doubleScan = kmsMode.timings.flags.doubleScan;
                mode->hSyncPos = kmsMode.timings.flags.hSyncPos;
                mode->hSyncNeg = kmsMode.timings.flags.hSyncNeg;
                mode->vSyncPos = kmsMode.timings.flags.vSyncPos;
                mode->vSyncNeg = kmsMode.timings.flags.vSyncNeg;
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
    NV_KMS_WINDOWS_SCANOUT_FORMAT format,
    NV_KMS_WINDOWS_SURFACE** surface,
    void** pixels,
    NvU32* pitch)
{
    if (width == 0 || height == 0 ||
        (format != NV_KMS_WINDOWS_SCANOUT_FORMAT_XRGB8 &&
         format != NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16 &&
         format != NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8) ||
        surface == nullptr ||
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
    const NvKmsSurfaceMemoryFormat kmsFormat =
        format == NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16
            ? NvKmsSurfaceMemoryFormatRF16GF16BF16AF16
            : format == NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8
                ? NvKmsSurfaceMemoryFormatA8R8G8B8
                : NvKmsSurfaceMemoryFormatX8R8G8B8;
    const NvU64 supportedFormats =
        format == NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8 ?
            resources.caps.supportedCursorSurfaceMemoryFormats :
            resources.supportedSurfaceMemoryFormats[0];
    if ((supportedFormats & NVBIT64(kmsFormat)) == 0 ||
        (format == NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8 &&
         (width != height || width > resources.caps.maxCursorSizeInPixels)))
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    const NvU32 alignment = resources.caps.pitchAlignment == 0 ?
        1 : resources.caps.pitchAlignment;
    const NvU64 bytesPerPixel =
        format == NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16 ? 8 : 4;
    const NvU64 rowBytes = static_cast<NvU64>(width) * bytesPerPixel;
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
    memoryParams.useVideoMemory = NV_TRUE;
    memoryParams.compressible = &compressible;
    scanout->memory = kmsFunctions.allocateMemory(kmsDevice, &memoryParams);
    if (scanout->memory == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, scanout);
        return NV_ERR_NO_MEMORY;
    }

    memoryParams.useVideoMemory = NV_FALSE;
    scanout->stagingMemory =
        kmsFunctions.allocateMemory(kmsDevice, &memoryParams);
    if (scanout->stagingMemory == nullptr)
    {
        kmsFunctions.freeMemory(kmsDevice, scanout->memory);
        HeapFree(GetProcessHeap(), 0, scanout);
        return NV_ERR_NO_MEMORY;
    }

    char memoryMessage[192] = {};
    nvkms_snprintf(
        memoryMessage,
        sizeof(memoryMessage),
        "NVKMS scanout memory: size=0x%llx vidmem=%u contiguous=%u",
        static_cast<unsigned long long>(allocationSize),
        kmsFunctions.isVidmem(scanout->memory),
        kmsFunctions.isContiguous(scanout->memory));
    NvKmsWindowsTraceMessage(memoryMessage);

    if (!kmsFunctions.mapMemory(
            kmsDevice, scanout->stagingMemory, NVKMS_KAPI_MAPPING_TYPE_USER,
            &scanout->pixels))
    {
        kmsFunctions.freeMemory(kmsDevice, scanout->stagingMemory);
        kmsFunctions.freeMemory(kmsDevice, scanout->memory);
        HeapFree(GetProcessHeap(), 0, scanout);
        return NV_ERR_GENERIC;
    }

    scanout->size = allocationSize;
    scanout->width = width;
    scanout->height = height;
    scanout->pitch = static_cast<NvU32>(alignedPitch);
    scanout->format = format;

    NvKmsKapiCreateSurfaceParams surfaceParams = {};
    surfaceParams.planes[0].memory = scanout->memory;
    surfaceParams.planes[0].pitch = scanout->pitch;
    surfaceParams.width = width;
    surfaceParams.height = height;
    surfaceParams.format = kmsFormat;
    surfaceParams.explicit_layout = NV_TRUE;
    surfaceParams.layout = NvKmsSurfaceMemoryLayoutPitch;
    scanout->surface = kmsFunctions.createSurface(kmsDevice, &surfaceParams);
    if (scanout->surface == nullptr)
    {
        kmsFunctions.unmapMemory(
            kmsDevice, scanout->stagingMemory, NVKMS_KAPI_MAPPING_TYPE_USER,
            scanout->pixels);
        kmsFunctions.freeMemory(kmsDevice, scanout->stagingMemory);
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
    DestroySurface(surface);
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

    const NV_STATUS lutStatus = EnsureDefaultLutSurface();
    if (lutStatus != NV_OK)
    {
        NvKmsWindowsTraceMessage("NVKMS default LUT upload failed");
        return lutStatus;
    }

    NvKmsKapiDisplayMode modes[NVKMS_KAPI_MAX_HEADS] = {};
    NvU32 displayHeadMasks[NVKMS_KAPI_MAX_HEADS] = {};
    for (NvU32 index = 0; index < outputCount; ++index)
    {
        if (outputs[index].surface == nullptr)
        {
            return NV_ERR_INVALID_ARGUMENT;
        }

        NV_KMS_WINDOWS_SURFACE* const surface = outputs[index].surface;
        if (!kmsFunctions.copyMemory(
                kmsDevice,
                surface->memory,
                surface->stagingMemory,
                surface->size))
        {
            NvKmsWindowsTraceMessage("NVKMS scanout upload failed");
            return NV_ERR_GENERIC;
        }

#if NV_KMS_WINDOWS_VERIFY_SCANOUT_UPLOAD
        const NvU32 uploadHash = HashSurface(surface->pixels, surface->size);
        const NvBool readbackSucceeded = kmsFunctions.copyMemory(
            kmsDevice,
            surface->stagingMemory,
            surface->memory,
            surface->size);
        const NvU32 readbackHash = readbackSucceeded ?
            HashSurface(surface->pixels, surface->size) : 0;
        char uploadMessage[160] = {};
        nvkms_snprintf(
            uploadMessage,
            sizeof(uploadMessage),
            "NVKMS scanout upload: size=0x%llx hash=0x%08x readback=%u readbackHash=0x%08x",
            surface->size,
            uploadHash,
            readbackSucceeded,
            readbackHash);
        NvKmsWindowsTraceMessage(uploadMessage);
#endif

        const NV_KMS_WINDOWS_MODE* const mode = &outputs[index].mode;
        NvKmsKapiDisplayModeTimings* const timings = &modes[index].timings;
        timings->refreshRate = mode->refreshRateMillihertz;
        timings->pixelClockHz = mode->pixelClockHz;
        timings->hVisible = mode->hVisible;
        timings->hSyncStart = mode->hSyncStart;
        timings->hSyncEnd = mode->hSyncEnd;
        timings->hTotal = mode->hTotal;
        timings->hSkew = mode->hSkew;
        timings->vVisible = mode->vVisible;
        timings->vSyncStart = mode->vSyncStart;
        timings->vSyncEnd = mode->vSyncEnd;
        timings->vTotal = mode->vTotal;
        timings->widthMM = mode->widthMillimeters;
        timings->heightMM = mode->heightMillimeters;
        timings->flags.interlaced = mode->interlaced;
        timings->flags.doubleScan = mode->doubleScan;
        timings->flags.hSyncPos = mode->hSyncPos;
        timings->flags.hSyncNeg = mode->hSyncNeg;
        timings->flags.vSyncPos = mode->vSyncPos;
        timings->flags.vSyncNeg = mode->vSyncNeg;
        nvkms_snprintf(
            modes[index].name,
            sizeof(modes[index].name),
            "%ux%u",
            mode->hVisible,
            mode->vVisible);

        if (outputs[index].surface->width != timings->hVisible ||
            outputs[index].surface->height != timings->vVisible)
        {
            return NV_ERR_INVALID_ARGUMENT;
        }

        NvKmsKapiDynamicDisplayParams dynamicInfo = {};
        dynamicInfo.handle = outputs[index].displayHandle;
        NvKmsKapiStaticDisplayInfo staticInfo = {};
        if (!kmsFunctions.getDynamicDisplayInfo(kmsDevice, &dynamicInfo) ||
            !dynamicInfo.connected ||
            !kmsFunctions.getStaticDisplayInfo(
                kmsDevice, outputs[index].displayHandle, &staticInfo) ||
            staticInfo.headMask == 0)
        {
            return NV_ERR_OBJECT_NOT_FOUND;
        }
        displayHeadMasks[index] = staticInfo.headMask;
    }

    NvU32 assignmentCount = 1;
    for (NvU32 index = 0; index < outputCount; ++index)
    {
        assignmentCount *= resources.numHeads;
    }

    for (NvU32 assignmentOffset = 0;
         assignmentOffset < assignmentCount;
         ++assignmentOffset)
    {
        const NvU32 encodedAssignment =
            assignmentCount - assignmentOffset - 1;
        NvU32 assignedHeads[NVKMS_KAPI_MAX_HEADS] = {};
        NvU32 remaining = encodedAssignment;
        NvU32 usedHeads = 0;
        bool assignmentValid = true;
        for (NvU32 index = 0; index < outputCount; ++index)
        {
            const NvU32 head = remaining % resources.numHeads;
            remaining /= resources.numHeads;
            if ((usedHeads & (1U << head)) != 0 ||
                (displayHeadMasks[index] & (1U << head)) == 0 ||
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
        requested.bypassComposition =
            NV_KMS_WINDOWS_BYPASS_COMPOSITION ? NV_TRUE : NV_FALSE;
        for (NvU32 index = 0; index < outputCount; ++index)
        {
            const NvU32 head = assignedHeads[index];
            requested.headsMask |= 1U << head;
            auto* const headConfig = &requested.headRequestedConfig[head];
            headConfig->modeSetConfig.bActive = NV_TRUE;
            headConfig->modeSetConfig.numDisplays = 1;
            headConfig->modeSetConfig.displays[0] = outputs[index].displayHandle;
            headConfig->modeSetConfig.mode = modes[index];
            headConfig->modeSetConfig.colorFormat =
                NVKMS_KAPI_REQUESTED_COLOR_FORMAT_RGB;
            headConfig->modeSetConfig.colorFormatSpecified = NV_TRUE;
            headConfig->modeSetConfig.colorBpc = outputs[index].colorBpc;
            headConfig->modeSetConfig.colorBpcSpecified = NV_TRUE;
            switch (outputs[index].colorSpace)
            {
            case NV_KMS_WINDOWS_COLOR_SPACE_HDR10:
                headConfig->modeSetConfig.hdrInfoFrame.enabled =
                    outputs[index].hdrMetadataEnabled;
                headConfig->modeSetConfig.hdrInfoFrame.eotf =
                    NVKMS_INFOFRAME_EOTF_ST2084;
                if (outputs[index].hdrMetadataEnabled)
                {
                    CopyHdrMetadata(
                        outputs[index].hdrMetadata,
                        &headConfig->modeSetConfig.hdrInfoFrame.staticMetadata);
                }
                headConfig->modeSetConfig.colorimetry =
                    NVKMS_OUTPUT_COLORIMETRY_BT2100;
                break;
            case NV_KMS_WINDOWS_COLOR_SPACE_SDR_WCG:
                headConfig->modeSetConfig.colorimetry =
                    NVKMS_OUTPUT_COLORIMETRY_BT709;
                break;
            case NV_KMS_WINDOWS_COLOR_SPACE_SDR:
            default:
                headConfig->modeSetConfig.colorimetry =
                    NVKMS_OUTPUT_COLORIMETRY_DEFAULT;
                break;
            }
            headConfig->modeSetConfig.olutFpNormScale =
                outputs[index].colorSpace == NV_KMS_WINDOWS_COLOR_SPACE_HDR10 ?
                NVKMS_OLUT_FP_NORM_SCALE_DEFAULT / 125 :
                NVKMS_OLUT_FP_NORM_SCALE_DEFAULT;
            headConfig->modeSetConfig.olut.enabled = NV_TRUE;
            if (headConfig->modeSetConfig.olut.enabled)
            {
                headConfig->modeSetConfig.olut.lutSurface =
                    kmsDefaultLutSurface->surface;
                headConfig->modeSetConfig.olut.offset =
                    outputs[index].colorSpace == NV_KMS_WINDOWS_COLOR_SPACE_HDR10 ?
                    static_cast<NvU32>(offsetof(LutData, pqOutput)) :
                    static_cast<NvU32>(offsetof(LutData, output));
                headConfig->modeSetConfig.olut.vssSegments =
                    outputs[index].colorSpace == NV_KMS_WINDOWS_COLOR_SPACE_HDR10 ?
                    pqLutSegments : 0;
                headConfig->modeSetConfig.olut.lutEntries =
                    outputs[index].colorSpace == NV_KMS_WINDOWS_COLOR_SPACE_HDR10 ?
                    pqLutEntries : LutEntries;
            }
            headConfig->flags.activeChanged = NV_TRUE;
            headConfig->flags.displaysChanged = NV_TRUE;
            headConfig->flags.modeChanged = NV_TRUE;
            headConfig->flags.hdrInfoFrameChanged = NV_TRUE;
            headConfig->flags.colorimetryChanged = NV_TRUE;
            headConfig->flags.legacyIlutChanged = NV_TRUE;
            headConfig->flags.legacyOlutChanged = NV_TRUE;
            headConfig->flags.olutChanged = NV_TRUE;
            headConfig->flags.olutFpNormScaleChanged = NV_TRUE;
            headConfig->flags.ditheringChanged = NV_TRUE;

            for (NvU32 layerIndex = 0;
                 layerIndex < resources.numLayers[head];
                 ++layerIndex)
            {
                auto* const layerConfig =
                    &headConfig->layerRequestedConfig[layerIndex];
                layerConfig->config.csc = NVKMS_IDENTITY_CSC_MATRIX;
                layerConfig->flags.surfaceChanged = NV_TRUE;
                layerConfig->flags.srcXYChanged = NV_TRUE;
                layerConfig->flags.srcWHChanged = NV_TRUE;
                layerConfig->flags.dstXYChanged = NV_TRUE;
                layerConfig->flags.dstWHChanged = NV_TRUE;
                layerConfig->flags.cscChanged = NV_TRUE;
                layerConfig->flags.inputTfChanged = NV_TRUE;
                layerConfig->flags.outputTfChanged = NV_TRUE;
                layerConfig->flags.inputColorSpaceChanged = NV_TRUE;
                layerConfig->flags.inputColorRangeChanged = NV_TRUE;
                layerConfig->flags.hdrMetadataChanged = NV_TRUE;
                layerConfig->flags.matrixOverridesChanged = NV_TRUE;
                layerConfig->flags.ilutChanged = NV_TRUE;
                layerConfig->flags.tmoChanged = NV_TRUE;
            }

            auto* const layer =
                &headConfig->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
            layer->config.surface = outputs[index].surface->surface;
            if (outputs[index].surface->format ==
                NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16)
            {
                layer->config.inputTf = NVKMS_INPUT_TF_LINEAR;
                layer->config.inputColorSpace = NVKMS_INPUT_COLOR_SPACE_SCRGB;
                layer->config.outputTf =
                    outputs[index].colorSpace == NV_KMS_WINDOWS_COLOR_SPACE_HDR10 ?
                    NVKMS_OUTPUT_TF_PQ : NVKMS_OUTPUT_TF_TRADITIONAL_GAMMA_SDR;
            }
            else
            {
                layer->config.inputTf = NVKMS_INPUT_TF_LINEAR;
                layer->config.inputColorSpace = NVKMS_INPUT_COLOR_SPACE_BT709;
                layer->config.outputTf = NVKMS_OUTPUT_TF_NONE;
                layer->config.ilut.enabled = NV_TRUE;
                layer->config.ilut.lutSurface = kmsDefaultLutSurface->surface;
                layer->config.ilut.offset =
                    static_cast<NvU32>(offsetof(LutData, base));
                layer->config.ilut.lutEntries = LutEntries;
            }
            layer->config.inputColorRange = NVKMS_INPUT_COLOR_RANGE_FULL;
            if (outputs[index].hdrMetadataEnabled)
            {
                layer->config.hdrMetadata.enabled = NV_TRUE;
                CopyHdrMetadata(outputs[index].hdrMetadata,
                                &layer->config.hdrMetadata.val);
            }
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

        char requestMessage[128] = {};
        nvkms_snprintf(
            requestMessage,
            sizeof(requestMessage),
            "NVKMS modeset request: outputs=%u headsMask=0x%08x",
            outputCount,
            requested.headsMask);
        NvKmsWindowsTraceMessage(requestMessage);

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
            outputs[index].surface->presented = NV_TRUE;
            gammaCache[assignedHeads[index]].valid = NV_FALSE;
            for (NvU32 layerIndex = 0;
                 layerIndex < resources.numLayers[assignedHeads[index]];
                 ++layerIndex)
            {
                layerCache[assignedHeads[index]][layerIndex] =
                    requested.headRequestedConfig[assignedHeads[index]].
                        layerRequestedConfig[layerIndex].config;
            }

            char assignmentMessage[128] = {};
            nvkms_snprintf(
                assignmentMessage,
                sizeof(assignmentMessage),
                "NVKMS head assignment: display=0x%08x mask=0x%08x head=%u",
                outputs[index].displayHandle,
                displayHeadMasks[index],
                assignedHeads[index]);
            NvKmsWindowsTraceMessage(assignmentMessage);
        }
        return NV_OK;
    }

    return NV_ERR_NOT_SUPPORTED;
}

extern "C" NV_STATUS NvKmsWindowsUpdateCursor(
    NvU32 head,
    NvU32 displayHandle,
    NV_KMS_WINDOWS_SURFACE* surface,
    NvBool surfaceChanged,
    NvS32 x,
    NvS32 y,
    NvU32 sdrWhiteLevel)
{
    if (displayHandle == 0 || x < -32768 || x > 32767 ||
        y < -32768 || y > 32767 || sdrWhiteLevel == 0 ||
        sdrWhiteLevel > 10000)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    KmsDeviceLockGuard lock;
    NvKmsKapiDeviceResourcesInfo resources = {};
    if (kmsDevice == nullptr)
    {
        return NV_ERR_INVALID_STATE;
    }
    if (!kmsFunctions.getDeviceResourcesInfo(kmsDevice, &resources) ||
        head >= resources.numHeads)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (surfaceChanged && surface != nullptr)
    {
        if (surface->format != NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8)
        {
            return NV_ERR_INVALID_ARGUMENT;
        }
        FlushProcessWriteBuffers();
        if (!kmsFunctions.copyMemory(
            kmsDevice, surface->memory, surface->stagingMemory,
                surface->size))
        {
            return NV_ERR_GENERIC;
        }
    }

    NvKmsKapiRequestedModeSetConfig requested = {};
    NvKmsKapiModeSetReplyConfig reply = {};
    requested.headsMask = 1U << head;
    auto& headConfig = requested.headRequestedConfig[head];
    headConfig.modeSetConfig.bActive = NV_TRUE;
    headConfig.modeSetConfig.numDisplays = 1;
    headConfig.modeSetConfig.displays[0] = displayHandle;
    auto& cursor = headConfig.cursorRequestedConfig;
    cursor.surface = surface == nullptr ? nullptr : surface->surface;
    cursor.compParams.compMode = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
    cursor.compParams.surfaceAlpha = 0;
    cursor.compParams.colorNormScale =
        FloatToFp16(static_cast<float>(sdrWhiteLevel) / 80.0F);
    cursor.dstX = static_cast<NvS16>(x);
    cursor.dstY = static_cast<NvS16>(y);
    cursor.flags.surfaceChanged = surfaceChanged;
    cursor.flags.dstXYChanged = NV_TRUE;
    if (!kmsFunctions.applyModeSetConfig(
            kmsDevice, &requested, &reply, NV_TRUE))
    {
        char message[128] = {};
        nvkms_snprintf(message, sizeof(message),
                      "NVKMS cursor failed: head=%u flipResult=%u image=%u",
                      head, reply.flipResult, surfaceChanged);
        NvKmsWindowsTraceMessage(message);
        return reply.flipResult == NV_KMS_FLIP_RESULT_IN_PROGRESS ?
            NV_ERR_IN_USE : NV_ERR_INVALID_STATE;
    }
    return NV_OK;
}

extern "C" NV_STATUS NvKmsWindowsPresentLayers(
    NvU32 head,
    NvU32 displayHandle,
    const NV_KMS_WINDOWS_LAYER* layers,
    NvU32 layerCount)
{
    return NvKmsWindowsPresentLayersTimed(
        head, displayHandle, layers, layerCount, NV_FALSE, nullptr);
}

extern "C" NV_STATUS NvKmsWindowsPresentLayersTimed(
    NvU32 head,
    NvU32 displayHandle,
    const NV_KMS_WINDOWS_LAYER* layers,
    NvU32 layerCount,
    NvBool preserveFrontBuffer,
    NV_KMS_WINDOWS_PRESENT_TIMING* timing)
{
    NV_KMS_WINDOWS_PRESENT_TIMING localTiming = {};
    if (timing == nullptr)
    {
        timing = &localTiming;
    }
    *timing = {};
    if (displayHandle == 0 ||
        (layers == nullptr && layerCount != 0) ||
        layerCount > NV_KMS_WINDOWS_MAX_LAYERS)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const NvU64 lockStart = PerformanceCounter();
    KmsDeviceLockGuard lock;
    timing->lockTicks = PerformanceCounter() - lockStart;
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

    if (preserveFrontBuffer)
    {
        const NvU64 pendingStart = PerformanceCounter();
        for (NvU32 layerIndex = 0; layerIndex < layerCount; ++layerIndex)
        {
            NvBool pending = NV_FALSE;
            if (layers[layerIndex].surface != nullptr &&
                layers[layerIndex].surface->presented &&
                !kmsFunctions.getFlipPendingStatus(kmsDevice, head, layerIndex, &pending))
            {
                return NV_ERR_INVALID_STATE;
            }
            timing->pendingTicks = PerformanceCounter() - pendingStart;
            if (pending)
            {
                return NV_ERR_IN_USE;
            }
            if (layers[layerIndex].surface != nullptr &&
                layerCache[head][layerIndex].surface == layers[layerIndex].surface->surface)
            {
                return NV_ERR_INVALID_STATE;
            }
        }
    }

    const NvU64 prepareStart = PerformanceCounter();
    NvKmsKapiRequestedModeSetConfig requested = {};
    NvKmsKapiModeSetReplyConfig reply = {};
    requested.headsMask = 1U << head;
    auto* const headConfig = &requested.headRequestedConfig[head];
    headConfig->modeSetConfig.bActive = NV_TRUE;
    headConfig->modeSetConfig.numDisplays = 1;
    headConfig->modeSetConfig.displays[0] = displayHandle;
    std::unique_ptr<NV_KMS_WINDOWS_SURFACE, decltype(&DestroySurface)>
        pendingGamma(nullptr, &DestroySurface);
    const NV_KMS_WINDOWS_COLOR_TRANSFORM* appliedGamma = nullptr;
    const auto* primaryTransform = layerCount != 0 ?
        layers[NVKMS_KAPI_LAYER_PRIMARY_IDX].colorTransform : nullptr;
    const NvBool resetGamma = layerCount != 0 && gammaCache[head].valid &&
        (primaryTransform == nullptr || !primaryTransform->gammaEnabled);
    if (resetGamma)
    {
        SetOutputLut(headConfig, kmsDefaultLutSurface,
                     layers[NVKMS_KAPI_LAYER_PRIMARY_IDX].colorSpace ==
                         NV_KMS_WINDOWS_COLOR_SPACE_HDR10);
    }

    for (NvU32 layerIndex = 0;
         layerIndex < resources.numLayers[head];
         ++layerIndex)
    {
        auto* const requestedLayer =
            &headConfig->layerRequestedConfig[layerIndex];
        const auto& previous = layerCache[head][layerIndex];
        requestedLayer->config = previous;
        if (layerIndex >= layerCount)
        {
            if (previous.surface != nullptr)
            {
                requestedLayer->config.surface = nullptr;
                requestedLayer->flags.surfaceChanged = NV_TRUE;
            }
            continue;
        }
        requestedLayer->flags.surfaceChanged = NV_TRUE;

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

        FlushProcessWriteBuffers();
        const NvU64 uploadStart = PerformanceCounter();
        if (!kmsFunctions.copyMemory(
                kmsDevice,
                layer->surface->memory,
                layer->surface->stagingMemory,
                layer->surface->size))
        {
            return NV_ERR_GENERIC;
        }
        timing->uploadTicks += PerformanceCounter() - uploadStart;

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

        switch (layer->colorSpace)
        {
        case NV_KMS_WINDOWS_COLOR_SPACE_HDR10:
            config->inputTf = NVKMS_INPUT_TF_LINEAR;
            config->inputColorSpace = NVKMS_INPUT_COLOR_SPACE_SCRGB;
            config->outputTf = NVKMS_OUTPUT_TF_PQ;
            break;
        case NV_KMS_WINDOWS_COLOR_SPACE_SDR_WCG:
            config->inputTf = NVKMS_INPUT_TF_LINEAR;
            config->inputColorSpace = NVKMS_INPUT_COLOR_SPACE_SCRGB;
            config->outputTf = NVKMS_OUTPUT_TF_TRADITIONAL_GAMMA_SDR;
            break;
        case NV_KMS_WINDOWS_COLOR_SPACE_SDR:
        default:
            config->inputTf = NVKMS_INPUT_TF_LINEAR;
            config->inputColorSpace = NVKMS_INPUT_COLOR_SPACE_BT709;
            config->outputTf = NVKMS_OUTPUT_TF_NONE;
            break;
        }
        config->inputColorRange = NVKMS_INPUT_COLOR_RANGE_FULL;

        config->hdrMetadata.enabled = layer->hdrMetadataEnabled;
        if (layer->hdrMetadataEnabled)
        {
            config->hdrMetadata.enabled = NV_TRUE;
            CopyHdrMetadata(layer->hdrMetadata, &config->hdrMetadata.val);
        }

        config->csc = NVKMS_IDENTITY_CSC_MATRIX;
        config->matrixOverrides.enabled.fmtCtm = NV_FALSE;
        if (layer->colorTransform != nullptr)
        {
            if (layer->colorTransform->cscEnabled)
            {
                const bool hdr = layer->colorSpace == NV_KMS_WINDOWS_COLOR_SPACE_HDR10;
                config->matrixOverrides.enabled.fmtCtm = hdr ? NV_TRUE : NV_FALSE;
                for (NvU32 row = 0; row < 3; ++row)
                {
                    for (NvU32 column = 0; column < 4; ++column)
                    {
                        auto& matrix = hdr ? config->matrixOverrides.fmtCtm : config->csc;
                        matrix.m[row][column] =
                            layer->colorTransform->csc[row][column];
                    }
                }
            }
            if (layerIndex == NVKMS_KAPI_LAYER_PRIMARY_IDX &&
                layer->colorTransform->gammaEnabled)
            {
                const HeadGammaCache& cached = gammaCache[head];
                if (!cached.valid ||
                    std::memcmp(
                        cached.red,
                        layer->colorTransform->gammaRed,
                        sizeof(cached.red)) != 0 ||
                    std::memcmp(
                        cached.green,
                        layer->colorTransform->gammaGreen,
                        sizeof(cached.green)) != 0 ||
                    std::memcmp(
                        cached.blue,
                        layer->colorTransform->gammaBlue,
                        sizeof(cached.blue)) != 0)
                {
                    NV_KMS_WINDOWS_SURFACE* surface = nullptr;
                    const NV_STATUS status =
                        CreateLutSurface(layer->colorTransform, &surface);
                    if (status != NV_OK)
                    {
                        return status;
                    }
                    pendingGamma.reset(surface);
                    SetOutputLut(headConfig, surface,
                                 layer->colorSpace ==
                                     NV_KMS_WINDOWS_COLOR_SPACE_HDR10);
                    appliedGamma = layer->colorTransform;
                }
            }
        }

        requestedLayer->flags.srcXYChanged =
            config->srcX != previous.srcX || config->srcY != previous.srcY;
        requestedLayer->flags.srcWHChanged =
            config->srcWidth != previous.srcWidth ||
            config->srcHeight != previous.srcHeight;
        requestedLayer->flags.dstXYChanged =
            config->dstX != previous.dstX || config->dstY != previous.dstY;
        requestedLayer->flags.dstWHChanged =
            config->dstWidth != previous.dstWidth ||
            config->dstHeight != previous.dstHeight;
        requestedLayer->flags.cscChanged =
            std::memcmp(&config->csc, &previous.csc, sizeof(config->csc)) != 0;
        requestedLayer->flags.matrixOverridesChanged =
            std::memcmp(&config->matrixOverrides, &previous.matrixOverrides,
                        sizeof(config->matrixOverrides)) != 0;
        requestedLayer->flags.inputTfChanged =
            config->inputTf != previous.inputTf;
        requestedLayer->flags.outputTfChanged =
            previous.surface == nullptr || config->outputTf != previous.outputTf;
        requestedLayer->flags.inputColorSpaceChanged =
            config->inputColorSpace != previous.inputColorSpace;
        requestedLayer->flags.inputColorRangeChanged =
            config->inputColorRange != previous.inputColorRange;
        requestedLayer->flags.hdrMetadataChanged =
            config->hdrMetadata.enabled != previous.hdrMetadata.enabled ||
            (config->hdrMetadata.enabled &&
             std::memcmp(&config->hdrMetadata.val, &previous.hdrMetadata.val,
                         sizeof(config->hdrMetadata.val)) != 0);
    }

    timing->prepareTicks = PerformanceCounter() - prepareStart - timing->uploadTicks;
    const NvU64 submitStart = PerformanceCounter();
    const NvBool committed = kmsFunctions.applyModeSetConfig(
        kmsDevice, &requested, &reply, NV_TRUE);
    timing->submitTicks = PerformanceCounter() - submitStart;
    if (!committed)
    {
        char failureMessage[128] = {};
        nvkms_snprintf(
            failureMessage,
            sizeof(failureMessage),
            "NVKMS present failed: head=%u flipResult=%u",
            head,
            reply.flipResult);
        NvKmsWindowsTraceMessage(failureMessage);
        return reply.flipResult == NV_KMS_FLIP_RESULT_IN_PROGRESS ?
            NV_ERR_IN_USE : NV_ERR_INVALID_STATE;
    }
    for (NvU32 layerIndex = 0;
         layerIndex < resources.numLayers[head];
         ++layerIndex)
    {
        layerCache[head][layerIndex] =
            headConfig->layerRequestedConfig[layerIndex].config;
        if (layerIndex < layerCount)
        {
            layers[layerIndex].surface->presented = NV_TRUE;
        }
    }
    if (appliedGamma != nullptr)
    {
        auto* const oldSurface = kmsGammaSurfaces[head];
        kmsGammaSurfaces[head] = pendingGamma.release();
        DestroySurface(oldSurface);
        std::memcpy(
            gammaCache[head].red,
            appliedGamma->gammaRed,
            sizeof(gammaCache[head].red));
        std::memcpy(
            gammaCache[head].green,
            appliedGamma->gammaGreen,
            sizeof(gammaCache[head].green));
        std::memcpy(
            gammaCache[head].blue,
            appliedGamma->gammaBlue,
            sizeof(gammaCache[head].blue));
        gammaCache[head].valid = NV_TRUE;
    }
    else if (resetGamma)
    {
        gammaCache[head].valid = NV_FALSE;
        DestroySurface(kmsGammaSurfaces[head]);
        kmsGammaSurfaces[head] = nullptr;
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

}
