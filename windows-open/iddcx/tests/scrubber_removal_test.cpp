#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using NvBool = bool;
using NvU16 = unsigned short;
using NvU32 = unsigned;
using NvU64 = unsigned long long;
using NvLength = size_t;
using NV_STATUS = int;
constexpr NvBool NV_TRUE = true, NV_FALSE = false;
constexpr NvU64 NV_U64_MAX = ~NvU64{0};
constexpr NV_STATUS NV_OK = 0, NV_ERR_INVALID_STATE = 1, NV_ERR_GPU_IS_LOST = 2;
constexpr NV_STATUS NV_ERR_TIMEOUT = 3;
constexpr unsigned GPU_TIMEOUT_DEFAULT = 0, GPU_TIMEOUT_FLAGS_BYPASS_THREAD_STATE = 1;
constexpr unsigned RMAPI_GPU_LOCK_INTERNAL = 0, TRANSFER_FLAGS_USE_BAR1 = 1, TRANSFER_FLAGS_NONE = 0;
constexpr unsigned SWL_SCRUBBER_CHANNEL = 0, CE_SCRUBBER_CHANNEL = 1;
constexpr unsigned MC_ENGINE_IDX_SEC2 = 0, MC_ENGINE_IDX_FIFO = 1, MC_ENGINE_IDX_VGPU = 2;
constexpr unsigned RM_ENGINE_TYPE_SEC2 = 0, NV_INTR_VECTOR_INVALID = ~0u;
constexpr unsigned ENGINE_INFO_TYPE_RM_ENGINE_TYPE = 0, ENGINE_INFO_TYPE_RUNLIST = 1;
unsigned checks = 0;
void Check(bool value, int line)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "Scrubber removal check %u failed at line %d\n", checks, line);
        std::exit(1);
    }
}
#define CHECK(value) Check((value), __LINE__)
struct Assertion {};
void Assert(bool value) { if (!value) throw Assertion{}; }
#define NV_ASSERT(value) Assert(value)
#define NV_ASSERT_OR_RETURN(value, status) do { if (!(value)) { NV_ASSERT(false); return status; } } while (0)
#define NV_ASSERT_OR_ELSE(value, action) do { if (!(value)) { NV_ASSERT(false); action; } } while (0)
#define NV_ASSERT_OK(value) NV_ASSERT((value) == NV_OK)
#define NV_ASSERT_OK_OR_ELSE(status, expression, action) \
    do { (status) = (expression); NV_ASSERT_OR_ELSE((status) == NV_OK, action); } while (0)
#define NV_ASSERT_OK_OR_CAPTURE_FIRST_ERROR(status, expression) \
    do { (status) = (expression); NV_ASSERT_OK(status); } while (0)
#define DBG_BREAKPOINT() NV_ASSERT(false)

enum class Point { None, Read, Timeout, Begin, Pending, Callback, ScrubCheck };
struct Metrics
{
    bool removed = false, reset = false, blocked = false, pending = false;
    Point point = Point::None;
    NV_STATUS timeoutStatus = NV_ERR_TIMEOUT, beginStatus = NV_OK, pendingStatus = NV_OK;
    unsigned reads = 0, readsAfterRemoval = 0, timeouts = 0, begins = 0;
    unsigned pendingQueries = 0, services = 0, ends = 0, callbacks = 0, traces = 0;
    unsigned transfers = 0, clientFrees = 0, spinDestructs = 0, mutexDestructs = 0;
    unsigned scrubQueries = 0, scrubChecks = 0, certificates = 0, unregisters = 0;
    unsigned removedReadValue = 0;
    std::vector<void*> freed;
} m;
void Fire(Point point) { if (m.point == point) { m.point = Point::None; m.removed = true; } }
NvBool NvKmsWindowsIsGpuSurpriseRemoved() { return m.removed; }
void NvKmsWindowsTraceMessage(const char*) { ++m.traces; }
template<typename... Args> void Print(Args...) {}
#define NV_PRINTF(level, ...) Print(__VA_ARGS__)

struct Intr {};
struct KernelFifo {};
struct KernelBus {};
struct MemoryManager {};
struct OBJGPU { unsigned gpuInstance = 0; Intr intr; KernelFifo fifo; KernelBus bus; MemoryManager memory; };
struct RMTIMEOUT {};
struct CALL_CONTEXT {};
using MC_ENGINE_BITVECTOR = unsigned;
#define GPU_GET_INTR(gpu) (&(gpu)->intr)
#define GPU_GET_KERNEL_FIFO(gpu) (&(gpu)->fifo)
#define GPU_GET_KERNEL_BUS(gpu) (&(gpu)->bus)
#define GPU_GET_MEMORY_MANAGER(gpu) (&(gpu)->memory)
#define MC_ENGINE_IDX_CE(value) (3u + (value))
#define MC_ENGINE_IDX_ESCHEDn(value) (8u + (value))
#define RM_ENGINE_TYPE_COPY(value) (3u + (value))
#define API_GPU_IN_RESET_SANITY_CHECK(gpu) (m.reset)
#define IS_FMODEL(gpu) false
void bitVectorClrAll(unsigned* value) { *value = 0; }
void bitVectorSet(unsigned* value, unsigned bit) { *value |= 1u << bit; }
void bitVectorClr(unsigned* value, unsigned bit) { *value &= ~(1u << bit); }
void bitVectorAnd(unsigned* value, const unsigned* a, const unsigned* b) { *value = *a & *b; }
bool rmDeviceGpuLockIsOwner(unsigned) { return true; }
bool kfifoIsLiteModeEnabled_HAL(OBJGPU*, KernelFifo*) { return false; }
bool kfifoIsHostEngineExpansionSupported(KernelFifo*) { return false; }
NV_STATUS kfifoEngineInfoXlate_HAL(OBJGPU*, KernelFifo*, unsigned, unsigned, unsigned, unsigned* out)
{ *out = 0; return NV_OK; }
bool kbusIsBarAccessBlocked(KernelBus*) { return m.blocked; }
void gpuSetTimeout(OBJGPU*, unsigned, RMTIMEOUT*, unsigned) {}
NV_STATUS gpuCheckTimeout(OBJGPU*, RMTIMEOUT*) { ++m.timeouts; Fire(Point::Timeout); return m.timeoutStatus; }
void osSchedule() { CHECK(false); }
NV_STATUS _intrServiceStallCommonCheckBegin(OBJGPU*, Intr*, CALL_CONTEXT**)
{
    ++m.begins;
    Fire(Point::Begin);
    return m.beginStatus != NV_OK ? m.beginStatus : m.removed ? NV_ERR_GPU_IS_LOST : NV_OK;
}
void _intrServiceStallCommonCheckEnd(OBJGPU*, Intr*, CALL_CONTEXT*) { ++m.ends; }
NV_STATUS intrGetPendingStall_HAL(OBJGPU*, Intr*, unsigned* engines, void*)
{
    ++m.pendingQueries;
    Fire(Point::Pending);
    *engines = ~0u;
    return m.pendingStatus;
}
bool intrIsPending_HAL(OBJGPU*, Intr*, NvU16, unsigned, void*)
{
    ++m.pendingQueries;
    Fire(Point::Pending);
    return true;
}
bool _intrServiceStallExactList(OBJGPU*, Intr*, unsigned*)
{
    CHECK(!m.removed);
    ++m.services;
    return false;
}
void intrServiceStallList_IMPL(OBJGPU*, Intr*, unsigned*, bool);
#define intrServiceStallList_HAL intrServiceStallList_IMPL

struct OBJCHANNEL
{
    OBJGPU* pGpu = nullptr;
    unsigned type = CE_SCRUBBER_CHANNEL, ceId = 0, lastSubmittedEntry = 0;
    unsigned channelNumGpFifioEntries = 4, payload = 10, get = 0;
    bool bUseBar1 = true, bClientUserd = true;
    void* pControlGPFifo = nullptr;
    void* pUserdMemdesc = nullptr;
    void* pbCpuVA = nullptr;
    void* pChannelBufferMemdesc = nullptr;
    void* pTokenFromNotifier = nullptr;
    void* pErrNotifierMemdesc = nullptr;
    unsigned hClient = 1;
};
unsigned Read(OBJCHANNEL* channel, bool payload)
{
    ++m.reads;
    if (m.removed) ++m.readsAfterRemoval;
    Fire(Point::Read);
    return m.removed ? m.removedReadValue : payload ? channel->payload : channel->get;
}
#define READ_CHANNEL_PAYLOAD_SEMA(channel) Read(channel, true)
#define READ_CHANNEL_PB_SEMA(channel) Read(channel, false)
struct CeUtilsCallback { NvU64 payload = 0; void (*pCallback)(void*) = nullptr; void* pArg = nullptr; };
struct CeUtilsCallbackList { std::vector<CeUtilsCallback*> values; };
void listInitIntrusive(CeUtilsCallbackList* list) { list->values.clear(); }
CeUtilsCallback* listHead(CeUtilsCallbackList* list) { return list->values.empty() ? nullptr : list->values.front(); }
void listRemove(CeUtilsCallbackList* list, CeUtilsCallback* entry)
{
    auto found = std::find(list->values.begin(), list->values.end(), entry);
    CHECK(found != list->values.end());
    list->values.erase(found);
}
void listAppendExisting(CeUtilsCallbackList* list, CeUtilsCallback* entry) { list->values.push_back(entry); }
size_t listCount(CeUtilsCallbackList* list) { return list->values.size(); }
void listDestroy(CeUtilsCallbackList* list) { CHECK(list->values.empty()); }
struct CeUtils
{
    OBJCHANNEL* pChannel = nullptr;
    OBJGPU* pGpu = nullptr;
    NvU64 lastSubmittedPayload = 10, lastCompletedPayload = 2;
    void* pCallbackLock = nullptr;
    CeUtilsCallbackList completionCallbacks;
};
void portSyncSpinlockAcquire(void*) {}
void portSyncSpinlockRelease(void*) {}
void portSyncSpinlockDestroy(void*) { CHECK(m.clientFrees == 1); ++m.spinDestructs; }
void portSyncMutexAcquire(void*) {}
void portSyncMutexRelease(void*) {}
void portSyncMutexDestroy(void*) { ++m.mutexDestructs; }
void portMemFree(void* pointer)
{
    if (!pointer) return;
    // Fixture storage owns backing; record the actual production release calls.
    CHECK(std::find(m.freed.begin(), m.freed.end(), pointer) == m.freed.end());
    m.freed.push_back(pointer);
}
void memmgrMemDescEndTransfer(MemoryManager*, void*, unsigned) { ++m.transfers; }
struct RM_API { NV_STATUS (*Free)(RM_API*, unsigned, unsigned); };
NV_STATUS FreeClient(RM_API*, unsigned, unsigned) { ++m.clientFrees; return NV_OK; }
RM_API api{FreeClient};
RM_API* rmapiGetInterface(unsigned) { return &api; }
NvU64 ceutilsUpdateProgress_IMPL(CeUtils*);
#define ceutilsUpdateProgress ceutilsUpdateProgress_IMPL
void ceutilsDestruct_IMPL(CeUtils*);
struct Sec2Utils {};
struct SCRUB_NODE {};
using PSCRUB_NODE = SCRUB_NODE*;
struct OBJMEMSCRUB
{
    void* pScrubberMutex = nullptr;
    void* pScrubList = nullptr;
    bool bIsEngineTypeSec2 = false;
    CeUtils* pCeUtils = nullptr;
    Sec2Utils* pSec2Utils = nullptr;
};
struct PMA { OBJMEMSCRUB* scrubber = nullptr; };
struct Heap { PMA* pPmaObject = nullptr; };
OBJMEMSCRUB* pmaGetMemScrub(PMA* pma) { return pma->scrubber; }
void pmaUnregMemScrub(PMA* pma) { ++m.unregisters; pma->scrubber = nullptr; }
bool _isScrubWorkPending(OBJMEMSCRUB*) { ++m.scrubQueries; return m.pending; }
SCRUB_NODE completedPages;
NV_STATUS _scrubCheckLocked(OBJMEMSCRUB*, PSCRUB_NODE* out, NvU64* count)
{
    ++m.scrubChecks;
    Fire(Point::ScrubCheck);
    *out = &completedPages;
    *count = 2;
    return NV_OK;
}
void pmaClearScrubbedPages(PMA*, PSCRUB_NODE, NvU64 count) { CHECK(count == 2); ++m.certificates; }
void objDelete(CeUtils* ce) { ceutilsDestruct_IMPL(ce); }
void objDelete(Sec2Utils*) { CHECK(false); }

#include "scrubber_removal_production.inc"

void Callback(void*) { CHECK(!m.removed); ++m.callbacks; Fire(Point::Callback); }
struct Fixture
{
    OBJGPU gpu;
    OBJCHANNEL channel;
    CeUtils ce;
    OBJMEMSCRUB scrubber;
    PMA pma;
    Heap heap;
    std::array<CeUtilsCallback, 3> entries;
    std::array<int, 6> storage{};
    Fixture()
    {
        m = {};
        channel.pGpu = &gpu;
        channel.pControlGPFifo = &storage[0]; channel.pUserdMemdesc = &storage[0];
        channel.pbCpuVA = &storage[1]; channel.pChannelBufferMemdesc = &storage[1];
        channel.pTokenFromNotifier = &storage[2]; channel.pErrNotifierMemdesc = &storage[2];
        ce.pChannel = &channel; ce.pGpu = &gpu; ce.pCallbackLock = &storage[3];
        for (unsigned i = 0; i != entries.size(); ++i)
        {
            entries[i] = {i + 1u, Callback, &storage[i]};
            ce.completionCallbacks.values.push_back(&entries[i]);
        }
        scrubber.pScrubberMutex = &storage[4]; scrubber.pScrubList = &storage[5]; scrubber.pCeUtils = &ce;
        pma.scrubber = &scrubber; heap.pPmaObject = &pma;
    }
    void CheckDestroyed(bool fullScrubber)
    {
        CHECK(m.clientFrees == 1 && m.spinDestructs == 1 && m.transfers == 3);
        CHECK(ce.completionCallbacks.values.empty());
        CHECK(channel.pControlGPFifo == nullptr && channel.pbCpuVA == nullptr && channel.pTokenFromNotifier == nullptr);
        CHECK(std::count(m.freed.begin(), m.freed.end(), &channel) == 1);
        for (auto& entry : entries) CHECK(std::count(m.freed.begin(), m.freed.end(), &entry) == 1);
        CHECK(m.freed.size() == 4u + (fullScrubber ? 3u : 0u) + (m.scrubChecks ? 1u : 0u));
        if (fullScrubber)
        {
            CHECK(m.unregisters == 1 && m.mutexDestructs == 1 && pma.scrubber == nullptr);
            CHECK(std::count(m.freed.begin(), m.freed.end(), &scrubber) == 1);
            CHECK(std::count(m.freed.begin(), m.freed.end(), scrubber.pScrubList) == 1);
            CHECK(std::count(m.freed.begin(), m.freed.end(), scrubber.pScrubberMutex) == 1);
        }
    }
};
template<typename Function> void ExpectAssert(Function function)
{
    bool asserted = false;
    try { function(); } catch (const Assertion&) { asserted = true; }
    CHECK(asserted);
}

int main()
{
#if defined(NV_SCRUBBER_TEST_BASELINE) || !defined(NV_WINDOWS_IDDCX)
    {
        Fixture f;
        m.removed = true;
        m.timeoutStatus = NV_OK;
        ExpectAssert([&] { ceutilsDestruct_IMPL(&f.ce); });
        CHECK(m.readsAfterRemoval == 1 && m.begins == 1 && m.clientFrees == 0);
    }
#endif
#if !defined(NV_SCRUBBER_TEST_BASELINE)
    {
        Fixture f;
        scrubberDestruct(&f.gpu, &f.heap);
        f.CheckDestroyed(true);
        CHECK(m.callbacks == 3 && f.ce.lastCompletedPayload == 10);
        CHECK(m.scrubChecks == 1 && m.certificates == 1 && m.readsAfterRemoval == 0);
    }
    for (bool single : {false, true})
    {
        Fixture f;
        if (single) intrServiceStallSingle_IMPL(&f.gpu, &f.gpu.intr, 3, false);
        else intrServiceStallList_IMPL(&f.gpu, &f.gpu.intr, nullptr, false);
        CHECK(m.begins == 1 && m.pendingQueries == 1 && m.services == 1 && m.ends == 1);
    }
    for (NV_STATUS status : {NV_ERR_GPU_IS_LOST, 99})
    {
        Fixture f;
        m.beginStatus = status;
        ExpectAssert([&] { intrServiceStallList_IMPL(&f.gpu, &f.gpu.intr, nullptr, false); });
        CHECK(m.services == 0 && m.ends == 0);
    }
    {
        Fixture f;
        f.channel.payload = 0;
        CHECK(channelWaitForFinishPayload(&f.channel, 10) == NV_ERR_TIMEOUT);
        f.channel.get = 1;
        unsigned output = 99;
        CHECK(channelWaitForFreeEntry(&f.channel, &output) == NV_ERR_TIMEOUT && output == 99);
#if defined(NV_WINDOWS_IDDCX)
        ceutilsDestruct_IMPL(&f.ce);
        CHECK(m.callbacks == 3 && f.ce.lastCompletedPayload == f.ce.lastSubmittedPayload);
#else
        ExpectAssert([&] { ceutilsDestruct_IMPL(&f.ce); });
#endif
    }
    {
        Fixture f;
        unsigned output = 99;
        CHECK(channelWaitForFreeEntry(&f.channel, &output) == NV_OK && output == 1);
        ExpectAssert([&] { channelWaitForFinishPayload(nullptr, 0); });
        ExpectAssert([&] { channelWaitForFreeEntry(nullptr, &output); });
    }
#if defined(NV_WINDOWS_IDDCX)
    for (Point point : {Point::None, Point::Read, Point::Timeout, Point::Begin, Point::Pending})
    {
        Fixture f;
        m.removed = point == Point::None;
        m.point = point;
        m.removedReadValue = ~0u;
        if (point != Point::Read) f.channel.payload = 0;
        if (point == Point::Begin || point == Point::Pending) m.timeoutStatus = NV_OK;
        ceutilsDestruct_IMPL(&f.ce);
        f.CheckDestroyed(false);
        CHECK(f.ce.lastCompletedPayload == 2 && m.callbacks == 0 && m.readsAfterRemoval == 0);
        CHECK(m.traces != 0);
        if (point == Point::None) CHECK(m.reads == 0 && m.begins == 0);
    }
    for (Point point : {Point::None, Point::Timeout, Point::ScrubCheck})
    {
        Fixture f;
        m.removed = point == Point::None;
        m.point = point;
        m.pending = point != Point::ScrubCheck;
        scrubberDestruct(&f.gpu, &f.heap);
        f.CheckDestroyed(true);
        CHECK(m.certificates == 0 && m.callbacks == 0 && f.ce.lastCompletedPayload == 2);
        CHECK(m.readsAfterRemoval == 0);
        if (point != Point::ScrubCheck) CHECK(m.scrubChecks == 0);
        if (point == Point::None) CHECK(m.scrubQueries == 0 && m.timeouts == 0);
    }
    for (bool single : {false, true})
    {
        for (Point point : {Point::None, Point::Begin, Point::Pending})
        {
            Fixture f;
            m.removed = point == Point::None; m.point = point;
            if (single) intrServiceStallSingle_IMPL(&f.gpu, &f.gpu.intr, 3, false);
            else intrServiceStallList_IMPL(&f.gpu, &f.gpu.intr, nullptr, false);
            CHECK(m.services == 0 && m.ends == (point == Point::Pending ? 1u : 0u));
            if (point == Point::None) CHECK(m.begins == 0 && m.pendingQueries == 0);
        }
        Fixture f;
        m.point = Point::Begin; m.beginStatus = 99;
        ExpectAssert([&] {
            if (single) intrServiceStallSingle_IMPL(&f.gpu, &f.gpu.intr, 3, false);
            else intrServiceStallList_IMPL(&f.gpu, &f.gpu.intr, nullptr, false);
        });
        CHECK(m.services == 0 && m.ends == 0);
    }
    for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IS_LOST, 99})
    {
        Fixture f;
        m.point = Point::Pending; m.pendingStatus = status;
        if (status == 99)
            ExpectAssert([&] { intrServiceStallList_IMPL(&f.gpu, &f.gpu.intr, nullptr, false); });
        else
            intrServiceStallList_IMPL(&f.gpu, &f.gpu.intr, nullptr, false);
        CHECK(m.services == 0 && m.ends == (status == 99 ? 0u : 1u));
    }
    for (bool payloadWait : {false, true})
    {
        for (bool removedAtEntry : {false, true})
        {
            Fixture f;
            m.removed = removedAtEntry;
            m.point = removedAtEntry ? Point::None : Point::Read;
            m.removedReadValue = ~0u;
            unsigned output = 99;
            NV_STATUS status = payloadWait ? channelWaitForFinishPayload(&f.channel, 10) :
                channelWaitForFreeEntry(&f.channel, &output);
            CHECK(status == NV_ERR_GPU_IS_LOST && output == 99);
            CHECK(m.readsAfterRemoval == 0 && m.begins == 0);
        }
    }
    for (bool removedAtEntry : {false, true})
    {
        Fixture f;
        m.removed = removedAtEntry;
        m.point = removedAtEntry ? Point::None : Point::Read;
        m.removedReadValue = ~0u;
        CHECK(ceutilsUpdateProgress_IMPL(&f.ce) == 2 && f.ce.lastCompletedPayload == 2);
        CHECK(m.readsAfterRemoval == 0);
    }
    {
        Fixture f;
        m.point = Point::Callback;
        ceutilsDestruct_IMPL(&f.ce);
        f.CheckDestroyed(false);
        CHECK(m.callbacks == 1 && f.ce.lastCompletedPayload == 10);
    }
    {
        Fixture f;
        f.ce.completionCallbacks.values.clear();
        m.removed = true;
        ceutilsDestruct_IMPL(&f.ce);
        CHECK(m.freed.size() == 1 && m.clientFrees == 1 && m.spinDestructs == 1);
        CHECK(m.reads == 0 && m.callbacks == 0 && f.ce.lastCompletedPayload == 2);
    }
#endif
#endif
    std::printf("Scrubber removal: %u checks passed\n", checks);
}
