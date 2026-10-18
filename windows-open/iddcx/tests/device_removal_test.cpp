#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <string>
#include <vector>

using NvU32 = unsigned;
using NvHandle = NvU32;
using NvU8 = unsigned char;
using NvU64 = unsigned long long;
using PHYSICAL_ADDRESS = LARGE_INTEGER;
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (static_cast<NTSTATUS>(status) >= 0)
#endif
using NvBool = bool;
using NV_STATUS = int;
constexpr NvU32 NV_U32_MAX = UINT_MAX;
constexpr NV_STATUS NV_OK = 0;
constexpr NV_STATUS NV_ERR_INVALID_STATE = 1;
constexpr NV_STATUS NV_ERR_INVALID_ARGUMENT = 2;
constexpr NV_STATUS NV_ERR_NO_INTR_PENDING = 3;
constexpr NV_STATUS NV_ERR_IN_USE = 4;
constexpr NV_STATUS NV_ERR_GPU_IS_LOST = 5;
constexpr NV_STATUS NV_ERR_GPU_IN_FULLCHIP_RESET = 6;
constexpr NV_STATUS NV_ERR_OBJECT_NOT_FOUND = 7;
constexpr NvBool NV_TRUE = true;
constexpr NvBool NV_FALSE = false;
constexpr int TRACE_LEVEL_INFORMATION = 0;
constexpr int TRACE_LEVEL_ERROR = 1;
constexpr int TRACE_LEVEL_WARNING = 2;
constexpr int TRACE_DRIVER = 0;
constexpr int RMAPI_LOCK_FLAGS_NONE = 0;
constexpr int RM_LOCK_MODULES_DESTROY = 0;
constexpr int GPUS_LOCK_FLAGS_NONE = 0;
constexpr ULONG PCI_WHICHSPACE_CONFIG = 0;
constexpr ULONG PciCommandOffset = 4;
constexpr USHORT PciCommandMemorySpace = 2;

unsigned cases = 0;
std::vector<std::string> calls;
unsigned hardwareCalls = 0;
unsigned dereferences = 0;
unsigned bulkReads = 0;
bool removeDuringBulk = false;
NV_STATUS apiStatus = NV_OK;
NV_STATUS visibilityStatus = NV_OK;
NV_STATUS publishedStatus = NV_OK;
NV_STATUS gpuLockStatus = NV_OK;

void Check(bool condition, int line)
{
    ++cases;
    if (!condition)
    {
        std::fprintf(stderr, "Device removal case %u failed at line %d\n", cases, line);
        std::exit(1);
    }
}
#define CHECK(condition) Check((condition), __LINE__)

template<typename... Args>
void TraceEvents(int, int, const char*, Args...) {}

struct OBJGPU { bool firmwareClient = true; bool virtualGpu = false; };
unsigned freeStatusWarnings = 0;
void NvKmsWindowsTraceMessage(const char*) { ++freeStatusWarnings; }
struct NV_PLATFORM_RM_GPU_CONTEXT { bool lost = false; bool shutdown = false; OBJGPU gpu; };
struct DeviceContext;
using WDFDEVICE = DeviceContext*;
using WDFOBJECT = DeviceContext*;
using WDFINTERRUPT = DeviceContext*;
using WDFCMRESLIST = void*;
using WDF_POWER_DEVICE_STATE = unsigned;

struct Output;
struct Adapter
{
    WDFDEVICE device_ = nullptr;
    Output* outputs_[2] = {};
    unsigned outputCount_ = 0;
    void ResetOutputs() noexcept;
    void Reset() { calls.emplace_back("outputs"); }
    NTSTATUS EnterD0(WDFDEVICE) { calls.emplace_back("enter"); return STATUS_SUCCESS; }
    void ExitD0() { calls.emplace_back("exit"); }
};
struct ResetOwner
{
    const char* name;
    void Reset() { calls.emplace_back(name); }
};
struct DeviceContext
{
    volatile LONG surpriseRemoved = 0;
    volatile LONG nvKmsWorkCanceled = 0;
    volatile LONG nvKmsWorkCompleted = 0;
    volatile LONG d0Active = 1;
    HANDLE nvKmsThread = nullptr;
    SRWLOCK lifecycleLock = SRWLOCK_INIT;
    bool rmGpuAttached = false;
    bool rmGpuStateLoaded = true;
    bool nvKmsInitialized = false;
    bool rmDeviceOpened = false;
    void* displayChangeWorkItem = nullptr;
    NvU32 rmGpuId = 1;
    NvU32 rmGpuInstance = 0;
    NvU32 rmDeviceInstance = 0;
    NvU32 rmClientHandle = 0;
    NvU32 rmDeviceHandle = 0;
    NvU32 rmSubdeviceHandle = 0;
    NV_PLATFORM_RM_GPU_CONTEXT* rmGpuContext = nullptr;
    Adapter adapter;
    ResetOwner deviceReset{"reset-interface"};
    ResetOwner interrupt{"interrupt"};
    ResetOwner dmaEngine{"dma"};
    ResetOwner bars{"bars"};
    ResetOwner configSpace{"config"};
    void* dmaRegistration = nullptr;
    void* pciConfigRegistration = nullptr;
};
DeviceContext* GetDeviceContext(WDFDEVICE device) { return device; }
WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT interrupt) { return interrupt; }
void WdfInterruptQueueDpcForIsr(WDFINTERRUPT) { ++hardwareCalls; }
void NvPlatformUnregisterDmaProvider(void*) { calls.emplace_back("dma-provider"); }
void NvPlatformUnregisterPciConfig(void*) { calls.emplace_back("pci-provider"); }
void DestroyRmGpuContext(DeviceContext* context)
{
    calls.emplace_back("rm-destroy");
    context->rmGpuContext = nullptr;
    context->rmGpuAttached = false;
}
NV_STATUS NvRmSetGpuInterruptEnabled(NV_PLATFORM_RM_GPU_CONTEXT*, bool)
{
    ++hardwareCalls;
    return NV_OK;
}
NV_STATUS NvRmServiceGpuInterrupt(NV_PLATFORM_RM_GPU_CONTEXT*)
{
    ++hardwareCalls;
    return NV_OK;
}
void NvPlatformRmGpuMarkLost(NV_PLATFORM_RM_GPU_CONTEXT* context)
{
    calls.emplace_back("platform-lost");
    context->lost = true;
}
NV_STATUS rmapiLockAcquire(int, int) { calls.emplace_back("api-lock"); return apiStatus; }
void rmapiLockRelease() { calls.emplace_back("api-unlock"); }
NV_STATUS gpumgrThreadEnableExpandedGpuVisibility()
{
    calls.emplace_back("visibility");
    return visibilityStatus;
}
void gpumgrThreadDisableExpandedGpuVisibility() { calls.emplace_back("visibility-end"); }
NV_STATUS NvPlatformRmGpuGetPublishedRmGpu(NV_PLATFORM_RM_GPU_CONTEXT* context, void** gpu)
{
    *gpu = &context->gpu;
    return publishedStatus;
}
NV_STATUS rmGpuLocksAcquire(int, int) { calls.emplace_back("gpu-lock"); return gpuLockStatus; }
void rmGpuLocksRelease(int, void*) { calls.emplace_back("gpu-unlock"); }
NV_STATUS osHandleGpuLost(OBJGPU*, bool emitXid)
{
    CHECK(!emitXid);
    calls.emplace_back("disconnected");
    return NV_OK;
}
NV_STATUS NvRmMarkGpuLost(NV_PLATFORM_RM_GPU_CONTEXT*);

struct Interrupt
{
    static NTSTATUS Enable(WDFINTERRUPT, WDFDEVICE);
    static NTSTATUS Disable(WDFINTERRUPT, WDFDEVICE);
    static BOOLEAN Isr(WDFINTERRUPT, ULONG);
    static void Dpc(WDFINTERRUPT, WDFOBJECT);
};

struct UmBusSpecificResetType { ULONGLONG value; };
struct UmBusSpecificResetFlags { ULONGLONG value; };
const GUID GUID_BUS_TYPE_PCI = {};
struct DeviceResetInterface
{
    WDFDEVICE device_ = nullptr;
    struct ResetInterface
    {
        void* context = nullptr;
        NTSTATUS (*deviceReset)(void*, ULONG, ULONG, void*) = nullptr;
        ULONG supportedResetTypes = 1;
        unsigned version = 3;
        NTSTATUS (*deviceBusSpecificReset)(
            void*, const GUID*, UmBusSpecificResetType, UmBusSpecificResetFlags*, void*) = nullptr;
    } interface_;
    ULONGLONG pciResetTypes_ = 0x20;
    void TraceResetStatus() const noexcept {}
    NTSTATUS ResetDevice(ULONG) noexcept;
    NTSTATUS ResetDeviceInPlace() noexcept;
};

struct ConfigSpace
{
    WDFDEVICE device_ = nullptr;
    struct BusInterface
    {
        void* Context = nullptr;
        ULONG (*GetBusData)(void*, ULONG, void*, ULONG, ULONG) = nullptr;
        ULONG (*SetBusData)(void*, ULONG, void*, ULONG, ULONG) = nullptr;
        void (*InterfaceDereference)(void*) = nullptr;
    } interface_;
    unsigned identity_ = 0;
    PHYSICAL_ADDRESS memoryBarAddresses_[3] = {};
    ULONG memoryBarCount_ = 0;
    bool identityValid_ = false;
    bool memorySpaceEnabledByDriver_ = true;
    void Reset() noexcept;
    NTSTATUS Read(ULONG, void*, ULONG) const noexcept;
    NTSTATUS Write(ULONG, const void*, ULONG) const noexcept;
    bool IsAvailable() const noexcept;
};

struct RegisterAperture
{
    WDFDEVICE device_;
    void* pseudoAddress_;
    SIZE_T length_ = 512;
    UCHAR ReadRegister8(SIZE_T) const noexcept;
    USHORT ReadRegister16(SIZE_T) const noexcept;
    ULONG ReadRegister32(SIZE_T) const noexcept;
    void ReadRegisterBuffer32(SIZE_T, ULONG*, ULONG) const noexcept;
    void WriteRegister8(SIZE_T, UCHAR) const noexcept;
    void WriteRegister16(SIZE_T, USHORT) const noexcept;
    void WriteRegister32(SIZE_T, ULONG) const noexcept;
    NTSTATUS Read32(SIZE_T, ULONG*) const noexcept;
    bool IsMapped() const noexcept { return true; }
};

template<typename T> T ReadRegister(WDFDEVICE, T* address)
{
    ++hardwareCalls;
    return *address;
}
template<typename T> void WriteRegister(WDFDEVICE, T* address, T value)
{
    ++hardwareCalls;
    *address = value;
}
void ReadBuffer(WDFDEVICE device, ULONG* address, ULONG* buffer, ULONG count)
{
    ++hardwareCalls;
    ++bulkReads;
    std::copy_n(address, count, buffer);
    if (removeDuringBulk) InterlockedExchange(&device->surpriseRemoved, 1);
}
#define WDF_READ_REGISTER_UCHAR ReadRegister<UCHAR>
#define WDF_READ_REGISTER_USHORT ReadRegister<USHORT>
#define WDF_READ_REGISTER_ULONG ReadRegister<ULONG>
#define WDF_READ_REGISTER_BUFFER_ULONG ReadBuffer
#define WDF_WRITE_REGISTER_UCHAR WriteRegister<UCHAR>
#define WDF_WRITE_REGISTER_USHORT WriteRegister<USHORT>
#define WDF_WRITE_REGISTER_ULONG WriteRegister<ULONG>

struct Output
{
    DeviceContext* device;
    unsigned scanoutResets = 0;
    bool monitorControlStopped = false;
    bool monitorDestroyed = false;
    bool removeOnStop = false;
    void StopMonitorControl() noexcept
    {
        monitorControlStopped = true;
        if (removeOnStop) InterlockedExchange(&device->surpriseRemoved, 1);
    }
    Output* Monitor() noexcept { return this; }
    bool HardwareRemoved() const noexcept { return device->surpriseRemoved != 0; }
    void RequireScanoutReset() { ++scanoutResets; }
    unsigned AssignedHead() const { return 0; }
    void* ScanoutSurface() const { return nullptr; }
    struct Display { unsigned displayHandle = 1; } display;
    const Display& DisplayState() const { return display; }
};
struct Scene
{
    DeviceContext* device;
    unsigned retired = 0;
    bool removeOnRetire = false;
    HRESULT Retire(bool* pending)
    {
        ++retired;
        *pending = false;
        if (removeOnRetire) InterlockedExchange(&device->surpriseRemoved, 1);
        return S_OK;
    }
};
struct Layer { void* surface = nullptr; };
struct D3D12_RANGE { SIZE_T Begin; SIZE_T End; };
struct Mapping
{
    unsigned unmaps = 0;
    void Unmap(unsigned, const D3D12_RANGE* written)
    {
        CHECK(written->Begin == 0 && written->End == 0);
        ++unmaps;
    }
};
struct Handle
{
    HANDLE value = nullptr;
    unsigned closes = 0;
    explicit operator bool() const { return value != nullptr; }
    HANDLE get() const { return value; }
    void reset()
    {
        if (value != nullptr)
        {
            CHECK(CloseHandle(value));
            value = nullptr;
            ++closes;
        }
    }
};
struct SwapChainUploadPipeline
{
    Output* output_;
    Scene* scene_;
    bool demoSubmitted_ = true;
    Layer desktop_;
    HANDLE stopEvent_ = nullptr;
    Handle thread_;
    Handle wake_;
    Handle animationTimer_;
    struct Job { Mapping* mapped = nullptr; };
    std::array<Job, 2> jobs_ = {};
    std::atomic<HRESULT> result_{S_OK};
    bool submitted_ = true;
    bool removeOnRefresh = false;
    unsigned refreshCount = 0;
    HRESULT Refresh()
    {
        ++refreshCount;
        if (removeOnRefresh) InterlockedExchange(&output_->device->surpriseRemoved, 1);
        return S_OK;
    }
    HRESULT DisableDemo() noexcept;
    HRESULT Stop() noexcept;
    void Fail(HRESULT result) { result_.store(result); }
};
NV_STATUS NvKmsWindowsGetFlipPending(unsigned, NvBool* pending)
{
    ++hardwareCalls;
    *pending = false;
    return NV_OK;
}
void NvKmsWindowsSetDisplayModes(void* outputs, unsigned count)
{
    CHECK(outputs == nullptr && count == 0);
    ++hardwareCalls;
}
void WdfObjectDelete(Output* output)
{
    CHECK(output->monitorControlStopped && !output->monitorDestroyed);
    output->monitorDestroyed = true;
}
NV_STATUS NvKmsWindowsPresentLayersTimed(unsigned, unsigned, Layer*, unsigned, NvBool, void*)
{
    ++hardwareCalls;
    return NV_OK;
}

SRWLOCK gpuRegistryLock = SRWLOCK_INIT;
NvBool gpuRegistered = false;
using NV_KMS_WINDOWS_GPU_REMOVED_CALLBACK = NvBool (*)(void*);
NV_KMS_WINDOWS_GPU_REMOVED_CALLBACK gpuRemovedCallback = nullptr;
void* gpuRemovedContext = nullptr;
struct GpuInfo
{
    NvU32 gpu_id = 0;
    struct { NvU32 domain = 0; NvU8 bus = 0, slot = 0, function = 0; } pci_info;
    NvBool needs_numa_setup = false;
    NvBool is_soc_disp = false;
    void* os_device_ptr = nullptr;
} registeredGpu;
NvBool NvKmsWindowsIsGpuSurpriseRemoved(void);
void NvKmsWindowsSetDisplayChangeCallback(void*, void*) { calls.emplace_back("callback-clear"); }
void WdfWorkItemFlush(void*) { calls.emplace_back("work-item"); }
void NvKmsWindowsShutdown() { calls.emplace_back("modeset-shutdown"); }
NV_STATUS NvRmCloseGpuDevice(NvU32) { calls.emplace_back("client-close"); return NV_OK; }
NV_STATUS NvRmUnloadGpuState(NV_PLATFORM_RM_GPU_CONTEXT*)
{
    calls.emplace_back("state-unload");
    return NV_OK;
}
NV_STATUS NvRmDetachGpuContext(NV_PLATFORM_RM_GPU_CONTEXT*, NvU32, NvU32, NvU32)
{
    CHECK(gpuRegistered && gpuRemovedCallback != nullptr && gpuRemovedContext != nullptr);
    calls.emplace_back("detach");
    return NV_OK;
}
void NvPlatformRmGpuMarkShutdown(NV_PLATFORM_RM_GPU_CONTEXT* context)
{
    context->shutdown = true;
    calls.emplace_back("shutdown");
}
void NvPlatformRmGpuContextDestroy(NV_PLATFORM_RM_GPU_CONTEXT*)
{
    CHECK(!gpuRegistered && gpuRemovedCallback == nullptr && gpuRemovedContext == nullptr);
    calls.emplace_back("context-destroy");
}

namespace ResourceFree
{
struct Assertion {};
void Assert(bool condition) { if (!condition) throw Assertion{}; }
struct RsResource { bool deleted = false; };
struct RsResourceRef;
struct Map { RsResourceRef* resource = nullptr; RsResourceRef* client = nullptr; };
struct RsResourceRef
{
    RsResource* pResource = nullptr;
    RsResourceRef* pParentRef = nullptr;
    bool bInvalidated = false;
    bool destroyed = false;
    NvU32 hResource = 2;
    NvU32 internalClassId = 0;
    Map childRefMap;
};
struct RsClient { Map resourceMap; int pendingFreeList = 0; NvU32 hClient = 1; };
struct RsServer {};
struct RS_RES_FREE_PARAMS_INTERNAL
{
    NvU32 hResource = 2;
    void* pLockInfo = nullptr;
    int* pSecInfo = nullptr;
    bool bInvalidateOnly = false;
};
struct CALL_CONTEXT
{
    RsClient* pClient = nullptr;
    RsResourceRef* pResourceRef = nullptr;
    RsServer* pServer = nullptr;
    void* pLockInfo = nullptr;
    int secInfo = 0;
};
NV_STATUS rpcStatus = NV_OK;
unsigned rpcCalls = 0, mappingCalls = 0, referenceFrees = 0;
RsResourceRef* mapFind(Map* map, NvU32 handle)
{
    return handle == 1 ? map->client : map->resource;
}
bool refPendingFree(RsResourceRef*, RsClient*) { return false; }
void listRemove(int*, RsResourceRef*) {}
void portMemSet(void* address, int value, size_t bytes) { std::memset(address, value, bytes); }
NV_STATUS resservSwapTlsCallContext(CALL_CONTEXT**, CALL_CONTEXT*) { return NV_OK; }
NV_STATUS resservRestoreTlsCallContext(CALL_CONTEXT*) { return NV_OK; }
void resSetFreeParams(RsResource*, CALL_CONTEXT*, RS_RES_FREE_PARAMS_INTERNAL*) {}
void resPreDestruct(RsResource*) {}
void clientUnmapResourceRefMappings(RsClient*, CALL_CONTEXT*, void*) { ++mappingCalls; }
void _clientUnmapBackRefMappings(RsClient*, CALL_CONTEXT*, void*) { ++mappingCalls; }
void _clientUnmapInterMappings(RsClient*, CALL_CONTEXT*, void*) { ++mappingCalls; }
void _clientUnmapInterBackRefMappings(RsClient*, CALL_CONTEXT*, void*) { ++mappingCalls; }
void _refRemoveAllDependencies(RsResourceRef*) {}
NV_STATUS serverFreeResourceRpcUnderLock(RsServer*, RS_RES_FREE_PARAMS_INTERNAL*)
{
    ++rpcCalls;
    return rpcStatus;
}
void objDelete(RsResource* resource) { resource->deleted = true; }
void multimapRemoveItemByKey(Map*, NvU32, NvU32) {}
void refUncacheRef(RsResourceRef*, RsResourceRef*) {}
NV_STATUS clientDestructResourceRef(RsClient*, RsServer*, RsResourceRef* reference, void*, int*)
{
    reference->destroyed = true;
    ++referenceFrees;
    return NV_OK;
}
struct MEMORY_DESCRIPTOR { bool freed = false; bool destroyed = false; };
struct Memory
{
    OBJGPU* pGpu;
    bool bConstructed = true;
    MEMORY_DESCRIPTOR* pMemDesc;
    bool bRpcAlloc = true;
    NvU32 hClient = 1, hParent = 2, hMemory = 3;
};
unsigned memoryFrees = 0, memoryRpcCalls = 0;
void memDestructCommon(Memory* memory) { ++memoryFrees; memory->bConstructed = false; }
void memdescFree(MEMORY_DESCRIPTOR* descriptor) { descriptor->freed = true; }
void memdescDestroy(MEMORY_DESCRIPTOR* descriptor) { descriptor->destroyed = true; }
NV_STATUS FreeMemoryRpc(OBJGPU* gpu, NvU32 client, NvU32 parent, NvU32 memory)
{
    CHECK(gpu != nullptr && client == 1 && parent == 2 && memory == 3);
    ++memoryRpcCalls;
    return rpcStatus;
}
}
#define NV_ASSERT(condition) ResourceFree::Assert(condition)
#define NV_ASSERT_OK(status) NV_ASSERT((status) == NV_OK)
#define NV_ASSERT_OK_OR_GOTO(status, expression, label) \
    do { (status) = (expression); if ((status) != NV_OK) { NV_ASSERT(false); goto label; } } while (0)
#define RES_GET_CLIENT_HANDLE(memory) ((memory)->hClient)
#define RES_GET_PARENT_HANDLE(memory) ((memory)->hParent)
#define RES_GET_HANDLE(memory) ((memory)->hMemory)
#define IS_VIRTUAL(gpu) ((gpu)->virtualGpu)
#define IS_FW_CLIENT(gpu) ((gpu)->firmwareClient)
#define NV_RM_RPC_FREE(gpu, client, parent, memory, status) \
    do { (status) = ResourceFree::FreeMemoryRpc(gpu, client, parent, memory); } while (0)

#if !defined(NV_REMOVAL_TEST_BASELINE)
namespace VaOwnership
{
struct EMEMBLOCK
{
    NvU64 begin = 0x120000000ULL, end = 0x120000FFFULL;
    NvU32 refCount = 1, owner = 0x1234;
};
struct OBJEHEAP
{
    EMEMBLOCK* block = nullptr;
    static EMEMBLOCK* eheapGetBlock(OBJEHEAP* heap, NvU64 address, NvU32 flags)
    {
        CHECK(address == 0x120000000ULL && flags == 0);
        return heap->block;
    }
};
struct OBJGVASPACE { OBJEHEAP* pHeap; NvU32 flags = 0; };
using OBJVASPACE = OBJGVASPACE;
unsigned internalFrees = 0;
NV_STATUS freeResult = NV_OK;
std::vector<std::string> messages;
void NvKmsWindowsTraceMessage(const char* message) { messages.emplace_back(message); }
template<typename... Args>
int nvDbgSnprintf(char* message, size_t size, const char* format, Args... args)
{
    int written = std::snprintf(message, size, format, args...);
    CHECK(written >= 0 && static_cast<size_t>(written) < size);
    return written;
}
NV_STATUS _gvaspaceInternalFree(OBJGVASPACE* vas, NvU64, EMEMBLOCK* block)
{
    ++internalFrees;
    if (freeResult == NV_OK)
    {
        if (block->refCount > 1) --block->refCount;
        else vas->pHeap->block = nullptr;
    }
    return freeResult;
}
struct OBJGPU { NvU32 gpuInstance = 0; NV_STATUS updateStatus = NV_OK; };
struct VirtMemAllocator {};
struct KernelMemorySystem {};
struct MemoryManager {};
struct MEMORY_DESCRIPTOR
{
    NvU64 address = 0x120000000ULL;
    NvU32 Allocated = 0;
    NvU64 Size = 0x2345, pageSize = 0x1000;
    NvU32 cacheAttrib = 0;
    OBJVASPACE* ownerVas = nullptr;
    bool destroyed = false;
};
struct Memory
{
    MEMORY_DESCRIPTOR* pMemDesc;
    NvU32 hClient = 1, hParent = 2, hMemory = 3;
    NvU32 categoryClassId = 0x50A0, HeapOwner = 0x1234;
    NvBool bConstructed = NV_TRUE, bRpcAlloc = NV_FALSE;
    OBJGPU* pGpu = nullptr;
};
struct VirtualMemory : Memory
{
    NvU32 hVASpace = 4;
    NvBool bReserveVaOnAlloc = NV_TRUE;
};
constexpr int AT_GPU = 0;
NvU64 memdescGetPhysAddr(MEMORY_DESCRIPTOR* descriptor, int addressType, int offset)
{
    CHECK(addressType == AT_GPU && offset == 0);
    return descriptor->address;
}
using GMMU_APERTURE = NvU32;
constexpr NvU32 NV01_MEMORY_VIRTUAL = 0x70;
constexpr NvU32 NVOS46_FLAGS_P2P_ENABLE_NONE = 0, NVOS46_FLAGS_P2P_ENABLE_SLI = 1;
constexpr NvU32 OS46_FLAGS_P2P_ENABLE_SHIFT = 0, OS46_FLAGS_P2P_ENABLE_MASK = 3;
constexpr NvU32 OS46_FLAGS_P2P_SUBDEV_ID_SRC_SHIFT = 4, OS46_FLAGS_P2P_SUBDEV_ID_SRC_MASK = 3;
constexpr NvU32 OS47_FLAGS_DEFER_TLB_INVALIDATION_SHIFT = 0, OS47_FLAGS_DEFER_TLB_INVALIDATION_MASK = 1;
constexpr NvU32 DMA_DEFER_TLB_INVALIDATE = 1, DMA_TLB_INVALIDATE = 0;
constexpr NvU32 VASPACE_FLAGS_SPARSIFIED = 1, DMA_UPDATE_VASPACE_FLAGS_UPDATE_VALID = 1;
constexpr NvU32 NV_MMU_PTE_VALID_FALSE = 0, GMMU_APERTURE_INVALID = 0, GMMU_APERTURE_PEER = 2;
constexpr NvU32 NV_MEMORY_CACHED = 1, FB_CACHE_PEER_MEMORY = 2, FB_CACHE_SYSTEM_MEMORY = 3;
constexpr NvU32 FB_CACHE_INVALIDATE = 1;
constexpr NvU64 NVLINK_INVALID_FABRIC_ADDR = ~NvU64{0};
struct CLI_DMA_MAPPING_INFO
{
    NvU32 Flags = 0;
    NvU64 mapPageSize = 0x1000;
    bool bNeedL2InvalidateAtUnmap = false;
    GMMU_APERTURE aperture = GMMU_APERTURE_PEER;
};
struct CLI_DMA_ALLOC_MAP_INFO
{
    VirtualMemory* pVirtualMemory;
    CLI_DMA_MAPPING_INFO* pDmaMappingInfo;
};
std::vector<OBJGPU*> sliGpus;
struct SliScope
{
    OBJGPU*& current;
    OBJGPU* saved;
    size_t index = 0;
    explicit SliScope(OBJGPU*& gpu) : current(gpu), saved(gpu) {}
    bool Next()
    {
        if (index == sliGpus.size()) return false;
        current = sliGpus[index++];
        return true;
    }
    ~SliScope() { current = saved; }
};
VirtMemAllocator allocator;
KernelMemorySystem memorySystem;
MemoryManager memoryManager;
unsigned updates = 0, cacheInvalidations = 0, unmapErrors = 0, commonDestructions = 0, ownerFrees = 0;
NvU64 expectedPageSize = 0x1000;
NvU32 expectedDefer = 0;
VirtMemAllocator* GetDma(OBJGPU*) { return &allocator; }
KernelMemorySystem* GetMemorySystem(OBJGPU*) { return &memorySystem; }
MemoryManager* GetMemoryManager(OBJGPU*) { return &memoryManager; }
NvU32 gpuGetDeviceInstance(OBJGPU*) { return 7; }
OBJGPU* gpumgrGetGpuFromSubDeviceInst(NvU32 device, NvU32 source)
{
    CHECK(device == 7 && source < sliGpus.size());
    return sliGpus[source];
}
MEMORY_DESCRIPTOR* memdescGetMemDescFromGpu(MEMORY_DESCRIPTOR* descriptor, OBJGPU*) { return descriptor; }
NvU64 memdescGetPageSize(MEMORY_DESCRIPTOR* descriptor, int) { return descriptor->pageSize; }
NvU32 memdescGetGpuP2PCacheAttrib(MEMORY_DESCRIPTOR* descriptor) { return descriptor->cacheAttrib; }
NvU32 vaspaceGetFlags(OBJVASPACE* vas) { return vas->flags; }
void LogUnmapError() { ++unmapErrors; }
void kmemsysCacheOp_HAL(OBJGPU*, KernelMemorySystem*, void*, GMMU_APERTURE aperture, NvU32 flags)
{
    CHECK((aperture == FB_CACHE_PEER_MEMORY || aperture == FB_CACHE_SYSTEM_MEMORY) &&
        flags == FB_CACHE_INVALIDATE);
    ++cacheInvalidations;
}
NV_STATUS dmaUpdateVASpace_HAL(OBJGPU* gpu, VirtMemAllocator*, OBJVASPACE*,
    MEMORY_DESCRIPTOR*, void* reserved, NvU64 low, NvU64 high, NvU32 flags,
    void* pages, void* compression, NvU32 kind, NvU32 valid, GMMU_APERTURE aperture,
    NvBool isVolatile, NvU32 target, NvU64 fabric, NvU32 defer, NvBool sparse, NvU64 pageSize)
{
    ++updates;
    CHECK(low == 0x120000000ULL);
    CHECK(high == low + ((0x123ULL + 0x2345 + pageSize - 1) & ~(pageSize - 1)) - 1);
    CHECK(flags == DMA_UPDATE_VASPACE_FLAGS_UPDATE_VALID && valid == NV_MMU_PTE_VALID_FALSE);
    CHECK(aperture == GMMU_APERTURE_INVALID && defer == expectedDefer && pageSize == expectedPageSize);
    CHECK(reserved == nullptr && pages == nullptr && compression == nullptr);
    CHECK(kind == 0 && !isVolatile && target == 0 && !sparse && fabric == NVLINK_INVALID_FABRIC_ADDR);
    return gpu->updateStatus;
}
NV_STATUS gvaspaceFree_IMPL(OBJGVASPACE*, NvU64);
NV_STATUS vaspaceFree(OBJVASPACE* vas, NvU64 address) { return gvaspaceFree_IMPL(vas, address); }
void memDestructCommon(Memory* memory) { ++commonDestructions; memory->bConstructed = NV_FALSE; }
void memdescDestroy(MEMORY_DESCRIPTOR* descriptor) { descriptor->destroyed = true; }
NV_STATUS memmgrFree(OBJGPU*, MemoryManager*, void*, NvHandle client, NvHandle parent,
    NvHandle vas, NvU32 owner, MEMORY_DESCRIPTOR* descriptor)
{
    CHECK(client == 1 && parent == 2 && vas == 4 && owner == 0x1234);
    ++ownerFrees;
    NV_STATUS status = gvaspaceFree_IMPL(descriptor->ownerVas, descriptor->address);
    memdescDestroy(descriptor);
    return status;
}
}
#define staticCast(pointer, type) static_cast<type*>(pointer)
#define NV_ASSERT_OR_RETURN(condition, status) \
    do { if (!(condition)) { NV_ASSERT(condition); return (status); } } while (0)
#define DRF_VAL(domain, reg, field, value) \
    (((value) >> domain##reg##field##_SHIFT) & domain##reg##field##_MASK)
#define SLI_LOOP_START(flags) for (VaOwnership::SliScope sliScope(pGpu); sliScope.Next();)
#define SLI_LOOP_END
#define SLI_LOOP_CONTINUE continue
#define GPU_GET_DMA(gpu) VaOwnership::GetDma(gpu)
#define GPU_GET_KERNEL_MEMORY_SYSTEM(gpu) VaOwnership::GetMemorySystem(gpu)
#define GPU_GET_MEMORY_MANAGER(gpu) VaOwnership::GetMemoryManager(gpu)
#define VAS_ADDRESS_TRANSLATION(vas) VaOwnership::AT_GPU
#define RM_ALIGN_UP(value, align) (((value) + (align) - 1) & ~((align) - 1))
#define RM_ALIGN_DOWN(value, align) ((value) & ~((align) - 1))
#define NV_PRINTF(level, ...) VaOwnership::LogUnmapError()
#endif

struct NVDmaBufferEvo
{
    NvU32 put_offset = 12;
    NvU32 get = 0;
};
using NVDmaBufferEvoPtr = NVDmaBufferEvo*;
struct EvoCore
{
    NVDmaBufferEvo pb;
    struct { volatile NvU32* cpuAddress; } notifierDma;
};
struct NVDevEvoRec { EvoCore* core; };
using NVDevEvoPtr = NVDevEvoRec*;
struct NVDispEvoRec { NVDevEvoPtr pDevEvo; NvU32 displayOwner = 0; };
constexpr NvU32 NV_EVO_NOTIFIER_SHORT_TIMEOUT_USEC = 3000000;
constexpr int EVO_LOG_WARN = 0;
#define nvAssert(condition) CHECK(condition)
#define DRF_SHIFTMASK(range) \
    ((~NvU32{0} >> (31 - ((1 ? range) - (0 ? range)))) << (0 ? range))
unsigned evoYields = 0;
unsigned evoWarnings = 0;
bool evoTimeout = false;
bool removeOnEvoYield = false;
bool completeOnEvoYield = false;
volatile NvU32* evoNotifier = nullptr;
NvU32 EvoCoreReadGet(NVDmaBufferEvoPtr buffer)
{
    ++hardwareCalls;
    return buffer->get;
}
NvBool nvExceedsTimeoutUSec(NVDevEvoPtr, NvU64*, NvU32 timeout)
{
    CHECK(timeout == NV_EVO_NOTIFIER_SHORT_TIMEOUT_USEC);
    return evoTimeout;
}
template<typename... Args> void nvEvoLogDisp(const NVDispEvoRec*, int, const char*, Args...)
{
    ++evoWarnings;
}
void nvkms_yield()
{
    CHECK(++evoYields <= 4);
    if (removeOnEvoYield)
        InterlockedExchange(&static_cast<DeviceContext*>(gpuRemovedContext)->surpriseRemoved, 1);
    if (completeOnEvoYield)
        *evoNotifier = 1;
}

#include "device_removal_production.inc"

ULONG ReadConfig(void*, ULONG, void* buffer, ULONG, ULONG length)
{
    ++hardwareCalls;
    std::memset(buffer, 0, length);
    return length;
}
ULONG WriteConfig(void*, ULONG, void*, ULONG, ULONG length)
{
    ++hardwareCalls;
    return length;
}
void Dereference(void*) { ++dereferences; }

void TestInterrupts()
{
    DeviceContext context;
    context.surpriseRemoved = 1;
    hardwareCalls = 0;
    CHECK(Interrupt::Disable(&context, &context) == STATUS_SUCCESS);
    CHECK(hardwareCalls == 0);
    CHECK(Interrupt::Enable(&context, &context) == STATUS_DEVICE_REMOVED);
    CHECK(!Interrupt::Isr(&context, 0));
    Interrupt::Dpc(&context, &context);
    CHECK(hardwareCalls == 0);
    context.surpriseRemoved = 0;
    CHECK(Interrupt::Enable(&context, &context) == STATUS_SUCCESS);
    CHECK(Interrupt::Disable(&context, &context) == STATUS_SUCCESS);
    CHECK(Interrupt::Isr(&context, 0));
    Interrupt::Dpc(&context, &context);
    CHECK(hardwareCalls == 4);
}

void TestHardwareAccess()
{
    DeviceContext context;
    ConfigSpace config;
    config.device_ = &context;
    config.interface_.GetBusData = ReadConfig;
    config.interface_.SetBusData = WriteConfig;
    config.interface_.InterfaceDereference = Dereference;
    ULONG registers[128];
    std::fill_n(registers, 128, 0x12345678UL);
    RegisterAperture aperture{&context, registers};
    ULONG value = 0;
    CHECK(config.Read(0, &value, sizeof(value)) == STATUS_SUCCESS);
    CHECK(config.Write(0, &value, sizeof(value)) == STATUS_SUCCESS);
    CHECK(aperture.ReadRegister32(0) == 0x12345678);
    CHECK(aperture.Read32(0, &value) == STATUS_SUCCESS && value == 0x12345678);
    aperture.WriteRegister8(0, 0x78);
    aperture.WriteRegister16(0, 0x5678);
    aperture.WriteRegister32(0, 0x12345678);
    CHECK(aperture.ReadRegister8(0) == 0x78);
    CHECK(aperture.ReadRegister16(0) == 0x5678);

    ULONG buffer[128] = {};
    aperture.ReadRegisterBuffer32(0, buffer, 128);
    CHECK(std::equal(std::begin(buffer), std::end(buffer), std::begin(registers)));
    bulkReads = 0;
    removeDuringBulk = true;
    aperture.ReadRegisterBuffer32(0, buffer, 128);
    CHECK(bulkReads == 1);
    CHECK(buffer[0] == 0x12345678 && buffer[63] == 0x12345678);
    CHECK(buffer[64] == MAXDWORD && buffer[127] == MAXDWORD);
    removeDuringBulk = false;
    hardwareCalls = 0;
    CHECK(config.Read(0, &value, sizeof(value)) == STATUS_DEVICE_REMOVED);
    CHECK(config.Write(0, &value, sizeof(value)) == STATUS_DEVICE_REMOVED);
    CHECK(aperture.ReadRegister8(0) == MAXBYTE);
    CHECK(aperture.ReadRegister16(0) == MAXWORD);
    CHECK(aperture.ReadRegister32(0) == MAXDWORD);
    CHECK(aperture.Read32(0, &value) == STATUS_DEVICE_REMOVED);
    aperture.ReadRegisterBuffer32(0, buffer, 128);
    CHECK(std::all_of(std::begin(buffer), std::end(buffer),
        [](ULONG element) { return element == MAXDWORD; }));
    aperture.WriteRegister8(0, 0);
    aperture.WriteRegister16(0, 0);
    aperture.WriteRegister32(0, 0);
    config.Reset();
    CHECK(hardwareCalls == 0);
    CHECK(registers[0] == 0x12345678);
    CHECK(dereferences == 1);
    CHECK(!config.IsAvailable() && config.device_ == nullptr);

    context.surpriseRemoved = 0;
    config.device_ = &context;
    config.interface_.GetBusData = ReadConfig;
    config.interface_.SetBusData = WriteConfig;
    config.interface_.InterfaceDereference = Dereference;
    config.memorySpaceEnabledByDriver_ = true;
    config.Reset();
    CHECK(hardwareCalls == 2);
    CHECK(dereferences == 2);
}

NTSTATUS ResetDevice(void*, ULONG, ULONG, void*) { ++hardwareCalls; return STATUS_SUCCESS; }
NTSTATUS ResetDeviceInPlace(void*, const GUID*, UmBusSpecificResetType,
    UmBusSpecificResetFlags*, void*)
{
    ++hardwareCalls;
    return STATUS_SUCCESS;
}

void TestReset()
{
    DeviceContext context;
    DeviceResetInterface reset;
    reset.device_ = &context;
    reset.interface_.deviceReset = ResetDevice;
    reset.interface_.deviceBusSpecificReset = ResetDeviceInPlace;
    hardwareCalls = 0;
    CHECK(reset.ResetDevice(0) == STATUS_SUCCESS);
    CHECK(reset.ResetDeviceInPlace() == STATUS_SUCCESS);
    CHECK(hardwareCalls == 2);
#if !defined(NV_REMOVAL_TEST_BASELINE)
    SurpriseRemoval(&context);
    CHECK(reset.ResetDevice(0) == STATUS_DEVICE_REMOVED);
    CHECK(reset.ResetDeviceInPlace() == STATUS_DEVICE_REMOVED);
    CHECK(hardwareCalls == 2);
#endif
}

#if !defined(NV_REMOVAL_TEST_BASELINE)
void TestNativeNotifierRemoval()
{
    NV_PLATFORM_RM_GPU_CONTEXT gpu;
    DeviceContext context;
    CHECK(NvKmsWindowsIsGpuRemoved());
    CHECK(!NvKmsWindowsIsGpuSurpriseRemoved());
    CHECK(NvKmsWindowsRegisterGpu(0, 0, 0, 0, 0, &gpu,
        IsNvKmsDeviceRemoved, &context) == NV_ERR_INVALID_ARGUMENT);
    CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, nullptr,
        IsNvKmsDeviceRemoved, &context) == NV_ERR_INVALID_ARGUMENT);
    CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, &gpu,
        nullptr, &context) == NV_ERR_INVALID_ARGUMENT);
    CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, &gpu,
        IsNvKmsDeviceRemoved, nullptr) == NV_ERR_INVALID_ARGUMENT);
    CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, &gpu,
        IsNvKmsDeviceRemoved, &context) == NV_OK);
    CHECK(!NvKmsWindowsIsGpuRemoved());
    gpu.lost = true;
    CHECK(!NvKmsWindowsIsGpuRemoved());
    CHECK(NvKmsWindowsRegisterGpu(2, 0, 0, 0, 0, &gpu,
        IsNvKmsDeviceRemoved, &context) == NV_ERR_IN_USE);
    NvKmsWindowsUnregisterGpu(2);
    CHECK(!NvKmsWindowsIsGpuRemoved());
    NvU32 notifier = 1;
    EvoCore core{{}, {&notifier}};
    NVDevEvoRec device{&core};
    NVDispEvoRec display{&device};
    CHECK(EvoCheckNotifier(&display, 0, 0, 0, 1, false));
    notifier = 0;
    CHECK(!EvoCheckNotifier(&display, 0, 0, 0, 1, false));
    evoNotifier = &notifier;
    completeOnEvoYield = true;
    CHECK(EvoCheckNotifier(&display, 0, 0, 0, 1, true));
    CHECK(evoYields == 1 && evoWarnings == 0 && notifier == 1);
    completeOnEvoYield = false;
    notifier = 0;
    evoYields = 0;
    removeOnEvoYield = true;
    hardwareCalls = 0;
    const ULONGLONG start = GetTickCount64();
    CHECK(!EvoCheckNotifier(&display, 0, 0, 0, 1, true));
    CHECK(GetTickCount64() - start < 250);
    CHECK(context.surpriseRemoved == 1 && evoYields == 1 && evoWarnings == 1);
    CHECK(NvKmsWindowsIsGpuSurpriseRemoved());
    CHECK(notifier == 0 && hardwareCalls == 0);
    removeOnEvoYield = false;
    auto* inaccessible = static_cast<NvU32*>(VirtualAlloc(
        nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    CHECK(inaccessible != nullptr);
    core.notifierDma.cpuAddress = inaccessible;
    CHECK(!EvoCheckNotifier(&display, 0, 0, 0, 1, false));
    CHECK(!EvoCheckNotifier(&display, 0, 0, 0, 1, true));
    CHECK(evoYields == 1 && hardwareCalls == 0);
    CHECK(VirtualFree(inaccessible, 0, MEM_RELEASE));
    core.notifierDma.cpuAddress = &notifier;
    context.surpriseRemoved = 0;
    evoYields = 0;
    evoTimeout = true;
    core.pb.get = core.pb.put_offset;
    CHECK(EvoCheckNotifier(&display, 0, 0, 0, 1, true));
    CHECK(notifier == 1 && hardwareCalls == 1 && evoYields == 0);
    evoTimeout = false;
    NvKmsWindowsUnregisterGpu(1);
    CHECK(NvKmsWindowsIsGpuRemoved());
    CHECK(!NvKmsWindowsIsGpuSurpriseRemoved());
    CHECK(gpuRemovedCallback == nullptr && gpuRemovedContext == nullptr &&
        registeredGpu.os_device_ptr == nullptr);
}

void TestNativeResourceFree()
{
    for (bool registered : {false, true})
    {
        for (bool removed : {false, true})
        {
            NV_PLATFORM_RM_GPU_CONTEXT gpu;
            gpu.lost = true;
            DeviceContext context;
            context.surpriseRemoved = removed ? 1 : 0;
            if (registered)
                CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, &gpu,
                    IsNvKmsDeviceRemoved, &context) == NV_OK);
            for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IN_FULLCHIP_RESET,
                NV_ERR_GPU_IS_LOST, NV_ERR_INVALID_ARGUMENT, 99})
            {
                bool expected = status == NV_OK || status == NV_ERR_GPU_IN_FULLCHIP_RESET;
#if defined(NV_WINDOWS_IDDCX)
                expected = expected || (status == NV_ERR_GPU_IS_LOST && registered && removed);
#endif
                freeStatusWarnings = 0;
                CHECK(serverIsFreeRpcStatusExpected(status) == expected);
                ResourceFree::RsResource resource;
                ResourceFree::RsResourceRef parent;
                ResourceFree::RsResourceRef reference;
                reference.pResource = &resource;
                reference.pParentRef = &parent;
                ResourceFree::RsClient client;
                client.resourceMap.resource = &reference;
                client.resourceMap.client = &parent;
                ResourceFree::RsServer server;
                ResourceFree::RS_RES_FREE_PARAMS_INTERNAL params;
                ResourceFree::rpcStatus = status;
                ResourceFree::rpcCalls = ResourceFree::mappingCalls =
                    ResourceFree::referenceFrees = 0;
                bool asserted = false;
                try
                {
                    CHECK(ResourceFree::clientFreeResource_IMPL(&client, &server, &params) == status);
                }
                catch (const ResourceFree::Assertion&) { asserted = true; }
                CHECK(asserted == !expected);
                CHECK(ResourceFree::rpcCalls == 1 && ResourceFree::mappingCalls == 4);
                CHECK(resource.deleted == expected && reference.destroyed == expected);
                CHECK((reference.pResource == nullptr) == expected);
                CHECK(ResourceFree::referenceFrees == (expected ? 1U : 0U));
                CHECK(freeStatusWarnings == (expected && status == NV_ERR_GPU_IS_LOST ? 2U : 0U));
            }
            if (registered) NvKmsWindowsUnregisterGpu(1);
        }
    }
}

void TestMemoryDestruction()
{
    for (bool removed : {false, true})
    {
        DeviceContext context;
        NV_PLATFORM_RM_GPU_CONTEXT gpu;
        context.surpriseRemoved = removed ? 1 : 0;
        CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, &gpu,
            IsNvKmsDeviceRemoved, &context) == NV_OK);
        for (bool rpcAllocation : {false, true})
        {
            for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IN_FULLCHIP_RESET, NV_ERR_GPU_IS_LOST, 99})
            {
                ResourceFree::MEMORY_DESCRIPTOR descriptor;
                ResourceFree::Memory memory{&gpu.gpu, true, &descriptor, rpcAllocation};
                ResourceFree::rpcStatus = status;
                ResourceFree::memoryFrees = ResourceFree::memoryRpcCalls = freeStatusWarnings = 0;
                bool expected = status == NV_OK || status == NV_ERR_GPU_IN_FULLCHIP_RESET;
#if defined(NV_WINDOWS_IDDCX)
                expected = expected || (status == NV_ERR_GPU_IS_LOST && removed);
#endif
                bool asserted = false;
                try { ResourceFree::memDestruct_IMPL(&memory); }
                catch (const ResourceFree::Assertion&) { asserted = true; }
                CHECK(asserted == (rpcAllocation && !expected));
                CHECK(ResourceFree::memoryFrees == 1 && !memory.bConstructed);
                CHECK(descriptor.freed && descriptor.destroyed);
                CHECK(ResourceFree::memoryRpcCalls == (rpcAllocation ? 1U : 0U));
                CHECK(freeStatusWarnings ==
                    (rpcAllocation && expected && status == NV_ERR_GPU_IS_LOST ? 1U : 0U));
            }
        }
        NvKmsWindowsUnregisterGpu(1);
    }
}

void TestVaOwnershipDiagnostics()
{
    using namespace VaOwnership;
    EMEMBLOCK block;
    OBJEHEAP heap{&block};
    OBJGVASPACE vas{&heap};
    for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IS_LOST, NV_ERR_INVALID_ARGUMENT})
    {
        heap.block = &block;
        freeResult = status;
        internalFrees = 0;
        messages.clear();
        CHECK(gvaspaceFree_IMPL(&vas, block.begin) == status);
        CHECK(internalFrees == 1 && heap.block == (status == NV_OK ? nullptr : &block) && block.refCount == 1);
#if defined(NV_WINDOWS_IDDCX) && defined(NV_RM_VERBOSE_DIAGNOSTICS)
        CHECK(messages.size() == 1 && messages[0].find("RM VA free:") == 0);
        CHECK(messages[0].find("va=0x120000000") != std::string::npos);
        CHECK(messages[0].find("refs=1") != std::string::npos);
#else
        CHECK(messages.empty());
#endif
    }
    heap.block = &block;
    messages.clear();
    CHECK(gvaspaceIncAllocRefCnt_IMPL(&vas, block.begin) == NV_OK && block.refCount == 2);
#if defined(NV_WINDOWS_IDDCX) && defined(NV_RM_VERBOSE_DIAGNOSTICS)
    CHECK(messages.size() == 1 && messages[0].find("add-reference") != std::string::npos);
    CHECK(messages[0].find("refs=2") != std::string::npos);
#else
    CHECK(messages.empty());
#endif
    heap.block = nullptr;
    internalFrees = 0;
    messages.clear();
    CHECK(gvaspaceIncAllocRefCnt_IMPL(&vas, block.begin) == NV_ERR_INVALID_ARGUMENT);
    CHECK(messages.empty() && block.refCount == 2);
    bool asserted = false;
    try { gvaspaceFree_IMPL(&vas, block.begin); }
    catch (const ResourceFree::Assertion&) { asserted = true; }
    CHECK(asserted && internalFrees == 0 && heap.block == nullptr);
#if defined(NV_WINDOWS_IDDCX) && defined(NV_RM_VERBOSE_DIAGNOSTICS)
    CHECK(messages.size() == 1 && messages[0].find("refs=0") != std::string::npos);
    _gvaspaceTraceAllocation("destroy-space", &vas, 0, nullptr);
    CHECK(messages.size() == 2 && messages.back().find("destroy-space") != std::string::npos);
    MEMORY_DESCRIPTOR descriptor;
    VirtualMemory memory{{&descriptor}};
    for (bool rpc : {false, true})
    {
        memory.bRpcAlloc = rpc;
        _virtmemTraceLifetime("destroy", &memory);
        const auto& message = messages.back();
        CHECK(message.find("RM virtual memory destroy:") == 0);
        CHECK(message.find(rpc ? "rpc=1" : "rpc=0") != std::string::npos);
        CHECK(message.find("va=0x120000000") != std::string::npos);
        CHECK(memory.pMemDesc == &descriptor && memory.bConstructed);
        CHECK(memory.bRpcAlloc == rpc && memory.bReserveVaOnAlloc);
        CHECK(memory.hVASpace == 4 && memory.HeapOwner == 0x1234);
        CHECK(descriptor.address == 0x120000000ULL && descriptor.Allocated == 0);
    }
    memory.pMemDesc = nullptr;
    _virtmemTraceLifetime("destroy", &memory);
    CHECK(messages.back().find("va=0x0") != std::string::npos);
    CHECK(memory.pMemDesc == nullptr && memory.bConstructed);
#endif
}

void TestReservedVaUnmapOwnership()
{
    using namespace VaOwnership;
    for (unsigned gpuCount : {1U, 2U})
    for (bool peer : {false, true})
    for (bool failSecond : {false, true})
    for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IS_LOST, NV_ERR_GPU_IN_FULLCHIP_RESET, 99})
    for (unsigned mappingKind : {0U, 1U, 2U})
    for (NvU64 pageSize : {0x1000ULL, 0x10000ULL})
    for (bool defer : {false, true})
    {
        if ((failSecond && gpuCount == 1) || (peer && mappingKind == 0)) continue;
        VaOwnership::OBJGPU first{0, failSecond ? NV_OK : status}, second{1, status};
        sliGpus = {&first};
        if (gpuCount == 2) sliGpus.push_back(&second);
        EMEMBLOCK block;
        OBJEHEAP heap{&block};
        OBJVASPACE vas{&heap};
        MEMORY_DESCRIPTOR virtualDescriptor;
        virtualDescriptor.ownerVas = &vas;
        MEMORY_DESCRIPTOR physicalDescriptor;
        physicalDescriptor.address = 0x123;
        physicalDescriptor.cacheAttrib = peer ? NV_MEMORY_CACHED : 0;
        VirtualMemory memory{{&virtualDescriptor}};
        memory.pGpu = &first;
        memory.bReserveVaOnAlloc = mappingKind == 2;
        if (!memory.bReserveVaOnAlloc) memory.categoryClassId = NV01_MEMORY_VIRTUAL;
        CLI_DMA_MAPPING_INFO mapping;
        mapping.Flags = peer ? NVOS46_FLAGS_P2P_ENABLE_SLI : NVOS46_FLAGS_P2P_ENABLE_NONE;
        mapping.mapPageSize = pageSize;
        mapping.bNeedL2InvalidateAtUnmap = defer;
        CLI_DMA_ALLOC_MAP_INFO info{&memory, &mapping};
        expectedPageSize = pageSize;
        expectedDefer = defer ? DMA_DEFER_TLB_INVALIDATE : DMA_TLB_INVALIDATE;
        internalFrees = updates = cacheInvalidations = unmapErrors = commonDestructions = ownerFrees = 0;
        freeResult = NV_OK;
        const bool reserved = mappingKind == 2;
        NV_STATUS expectedStatus = NV_OK;
        unsigned expectedUpdates = 0;
        if (reserved)
        {
            for (auto* gpu : sliGpus)
            {
                if (peer && gpu == &first) continue;
                ++expectedUpdates;
                expectedStatus = gpu->updateStatus;
                if (expectedStatus != NV_OK) break;
            }
        }
        CHECK(dmaFreeMapping_GM107(&first, &allocator, &vas,
            block.begin + (reserved ? 0x123 : 0), &physicalDescriptor,
            defer ? 1U : 0U, mappingKind == 0 ? nullptr : &info) == expectedStatus);
        CHECK(updates == expectedUpdates && unmapErrors == (expectedStatus == NV_OK ? 0U : 1U));
        const unsigned expectedCache = expectedStatus != NV_OK ? 0 :
            ((peer ? gpuCount : 0) + (defer && mappingKind != 0 ? 1U : 0));
        CHECK(cacheInvalidations == expectedCache);
        bool prematurelyFreed = false;
#if defined(NV_REMOVAL_TEST_DMA_BASELINE) || !defined(NV_DOWNSTREAM_BUGFIXES)
        prematurelyFreed = reserved && expectedStatus != NV_OK;
#endif
        CHECK(internalFrees == (!reserved || prematurelyFreed ? 1U : 0U));
        CHECK((heap.block == &block) == (reserved && !prematurelyFreed));
        if (mappingKind != 0)
        {
            bool asserted = false;
            try { virtmemDestruct_IMPL(&memory); }
            catch (const ResourceFree::Assertion&) { asserted = true; }
            CHECK(asserted == prematurelyFreed);
            CHECK(!memory.bConstructed && commonDestructions == 1);
            CHECK(virtualDescriptor.destroyed == !prematurelyFreed);
            CHECK(ownerFrees == (reserved ? 1U : 0U));
            CHECK(internalFrees == 1 && heap.block == nullptr);
        }
    }
}

void TestGpuDetachLifetime()
{
    for (bool removed : {false, true})
    {
        DeviceContext context;
        NV_PLATFORM_RM_GPU_CONTEXT gpu;
        context.surpriseRemoved = removed ? 1 : 0;
        context.rmGpuContext = &gpu;
        context.rmGpuAttached = context.nvKmsInitialized = context.rmDeviceOpened = true;
        context.displayChangeWorkItem = &context;
        CHECK(NvKmsWindowsRegisterGpu(1, 0, 0, 0, 0, &gpu,
            IsNvKmsDeviceRemoved, &context) == NV_OK);
        calls.clear();
        DestroyRmGpuContextForTest(&context);
        CHECK(!context.rmGpuAttached && !context.nvKmsInitialized && !context.rmDeviceOpened);
        CHECK(!context.rmGpuStateLoaded && context.rmGpuContext == nullptr && gpu.shutdown);
        CHECK(gpu.lost == removed);
        CHECK(context.rmGpuInstance == UINT_MAX && context.rmDeviceInstance == UINT_MAX);
        CHECK(context.rmGpuId == 0 && !NvKmsWindowsIsGpuSurpriseRemoved());
        const auto detach = std::find(calls.begin(), calls.end(), "detach");
        const auto destroy = std::find(calls.begin(), calls.end(), "context-destroy");
        CHECK(detach != calls.end() && destroy != calls.end() && detach < destroy);
        CHECK(std::count(calls.begin(), calls.end(), "modeset-shutdown") == 1);
        CHECK(std::count(calls.begin(), calls.end(), "client-close") == 1);
        CHECK(std::count(calls.begin(), calls.end(), "state-unload") == 1);
    }
}

void TestOutputReset()
{
    for (unsigned stage = 0; stage < 4; ++stage)
    {
        DeviceContext context;
        context.surpriseRemoved = stage == 1 ? 1 : 0;
        Output first{&context};
        Output second{&context};
        first.removeOnStop = stage == 2;
        Adapter adapter;
        adapter.device_ = &context;
        adapter.outputs_[0] = &first;
        adapter.outputs_[1] = &second;
        adapter.outputCount_ = stage == 3 ? 0 : 2;
        if (stage == 3)
            adapter.outputs_[0] = adapter.outputs_[1] = nullptr;
        hardwareCalls = 0;
        adapter.ResetOutputs();
        CHECK(adapter.outputCount_ == 0);
        CHECK(adapter.outputs_[0] == nullptr && adapter.outputs_[1] == nullptr);
        CHECK(first.monitorControlStopped == (stage != 3));
        CHECK(second.monitorControlStopped == (stage != 3));
        CHECK(first.monitorDestroyed == (stage != 3));
        CHECK(second.monitorDestroyed == (stage != 3));
        CHECK(hardwareCalls == (stage == 0 ? 1U : 0U));
    }
}

void TestLifecycle()
{
    DeviceContext context;
    AcquireSRWLockExclusive(&context.lifecycleLock);
    SurpriseRemoval(&context);
    ReleaseSRWLockExclusive(&context.lifecycleLock);
    CHECK(IsHardwareRemoved(&context));
    CHECK(context.nvKmsWorkCanceled == 1 && context.d0Active == 0);
    calls.clear();
    CHECK(EnterD0(&context, 0) == STATUS_DEVICE_REMOVED);
    CHECK(calls.empty());
    NV_PLATFORM_RM_GPU_CONTEXT gpu;
    context.rmGpuContext = &gpu;
    context.rmGpuAttached = true;
    CHECK(ReleaseHardware(&context, nullptr) == STATUS_SUCCESS);
    CHECK(gpu.lost);
    auto disconnected = std::find(calls.begin(), calls.end(), "disconnected");
    auto outputs = std::find(calls.begin(), calls.end(), "outputs");
    auto destroy = std::find(calls.begin(), calls.end(), "rm-destroy");
    auto bars = std::find(calls.begin(), calls.end(), "bars");
    CHECK(disconnected < outputs && outputs < destroy && destroy < bars);
    CHECK(context.rmGpuContext == nullptr);
    SurpriseRemoval(&context);
    CHECK(ReleaseHardware(&context, nullptr) == STATUS_SUCCESS);

    DeviceContext normal;
    normal.rmGpuContext = &gpu;
    normal.rmGpuAttached = true;
    calls.clear();
    CHECK(EnterD0(&normal, 0) == STATUS_SUCCESS);
    CHECK(ReleaseHardware(&normal, nullptr) == STATUS_SUCCESS);
    CHECK(std::find(calls.begin(), calls.end(), "disconnected") == calls.end());
    CHECK(normal.d0Active == 0);
    SurpriseRemoval(&normal);
    CHECK(IsHardwareRemoved(&normal));
}

void TestRmErrors()
{
    NV_PLATFORM_RM_GPU_CONTEXT gpu;
    CHECK(NvRmMarkGpuLost(nullptr) == NV_ERR_INVALID_ARGUMENT);
    for (unsigned stage = 0; stage < 4; ++stage)
    {
        apiStatus = stage == 0 ? 99 : NV_OK;
        visibilityStatus = stage == 1 ? 99 : NV_OK;
        publishedStatus = stage == 2 ? 99 : NV_OK;
        gpuLockStatus = stage == 3 ? 99 : NV_OK;
        calls.clear();
        CHECK(NvRmMarkGpuLost(&gpu) == 99);
        CHECK(gpu.lost);
        CHECK(std::find(calls.begin(), calls.end(), "disconnected") == calls.end());
        CHECK((std::count(calls.begin(), calls.end(), "api-unlock") == 1) == (stage != 0));
        CHECK((std::count(calls.begin(), calls.end(), "visibility-end") == 1) == (stage >= 2));
        CHECK(std::count(calls.begin(), calls.end(), "gpu-unlock") == 0);
    }
    apiStatus = visibilityStatus = publishedStatus = gpuLockStatus = NV_OK;
}

void TestRetirement()
{
    DeviceContext context;
    Output output{&context};
    Scene scene{&context};
    SwapChainUploadPipeline pipeline{&output, &scene};
    hardwareCalls = 0;
    CHECK(pipeline.DisableDemo() == S_OK);
    CHECK(scene.retired == 2 && pipeline.refreshCount == 1);
    CHECK(hardwareCalls == 3 && !pipeline.demoSubmitted_);
    for (unsigned stage = 0; stage < 3; ++stage)
    {
        context.surpriseRemoved = stage == 0 ? 1 : 0;
        scene.removeOnRetire = stage == 1;
        scene.retired = 0;
        pipeline.removeOnRefresh = stage == 2;
        pipeline.refreshCount = 0;
        pipeline.demoSubmitted_ = true;
        hardwareCalls = 0;
        CHECK(pipeline.DisableDemo() == S_OK);
        CHECK(context.surpriseRemoved == 1);
        CHECK(scene.retired == (stage == 0 ? 0U : 1U));
        CHECK(hardwareCalls == (stage == 2 ? 1U : 0U));
        CHECK(pipeline.demoSubmitted_);
    }
}

DWORD WINAPI WaitForStop(void* event)
{
    return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 ? 0 : 1;
}

void TestPipelineStop()
{
    for (bool removed : {false, true})
    {
        DeviceContext context;
        context.surpriseRemoved = removed ? 1 : 0;
        Output output{&context};
        Scene scene{&context};
        SwapChainUploadPipeline pipeline{&output, &scene};
        Mapping mapping;
        pipeline.jobs_[0].mapped = &mapping;
        pipeline.stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pipeline.wake_.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pipeline.animationTimer_.value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        pipeline.thread_.value = CreateThread(nullptr, 0, WaitForStop, pipeline.stopEvent_, 0, nullptr);
        CHECK(pipeline.stopEvent_ != nullptr && pipeline.wake_ &&
            pipeline.animationTimer_ && pipeline.thread_);
        hardwareCalls = 0;
        CHECK(pipeline.Stop() == S_OK);
        CHECK(mapping.unmaps == 1 && pipeline.jobs_[0].mapped == nullptr);
        CHECK(pipeline.thread_.closes == 1 && pipeline.animationTimer_.closes == 1);
        CHECK(output.scanoutResets == 0);
        CHECK(hardwareCalls == (removed ? 0U : 4U));
        CHECK(pipeline.output_ == nullptr);
        CHECK(pipeline.Stop() == S_OK && mapping.unmaps == 1);
        CHECK(CloseHandle(pipeline.stopEvent_));
        pipeline.wake_.reset();
    }
}
#endif

int main()
{
    TestInterrupts();
    TestHardwareAccess();
    TestReset();
#if !defined(NV_REMOVAL_TEST_BASELINE)
#if defined(NV_WINDOWS_IDDCX)
    TestNativeNotifierRemoval();
#endif
    TestNativeResourceFree();
    TestMemoryDestruction();
    TestVaOwnershipDiagnostics();
    TestReservedVaUnmapOwnership();
    TestGpuDetachLifetime();
    TestOutputReset();
    TestLifecycle();
    TestRmErrors();
    TestRetirement();
    TestPipelineStop();
#endif
    std::printf("Device removal: %u checks passed\n", cases);
}
