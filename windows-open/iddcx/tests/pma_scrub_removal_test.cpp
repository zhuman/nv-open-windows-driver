#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

using NvBool = bool;
using NvU32 = unsigned;
using NvU64 = unsigned long long;
using NvLength = size_t;
using NV_STATUS = int;
using RmPhysAddr = NvU64;
using PMA_PAGESTATUS = NvU32;
constexpr bool NV_TRUE = true, NV_FALSE = false;
constexpr NV_STATUS NV_OK = 0, NV_ERR_GPU_IS_LOST = 2, NV_ERR_TIMEOUT = 3;
constexpr NV_STATUS NV_ERR_INVALID_STATE = 4, NV_ERR_INVALID_ARGUMENT = 5, NV_ERR_NO_MEMORY = 6;
constexpr NV_STATUS NV_ERR_GENERIC = 7;
constexpr unsigned MAX_SCRUB_ITEMS = 4, GPU_TIMEOUT_DEFAULT = 0, PMA_PAGE_SHIFT = 16;
constexpr unsigned STATE_FREE = 0, ATTRIB_SCRUBBING = 8, ATTRIB_EVICTING = 4, ATTRIB_BLACKLIST = 64;
constexpr unsigned LOCALIZATION_STATE_IS_LOCALIZED = 1, PMA_FREE_SKIP_SCRUB = 1;
constexpr unsigned PMA_SCRUBBER_VALID = 1, PMA_SCRUBBER_INVALID = 0;
constexpr unsigned SCRUBBER_SUBMIT_FLAGS_LOCALIZED_SCRUB = 1, PMA_QUERY_SCRUB_ENABLED = 1;
constexpr unsigned PMA_QUERY_SCRUB_VALID = 2, PMA_QUERY_NUMA_ENABLED = 4, PMA_QUERY_NUMA_ONLINED = 8;
constexpr NvU64 _PMA_64KB = 1ull << 16, _PMA_128KB = 1ull << 17;
constexpr NvU64 _PMA_2MB = 1ull << 21, _PMA_512MB = 1ull << 29, SCRUB_MAX_BYTES_PER_LINE = 0xffffffffull;
constexpr unsigned ADDR_FBMEM = 0, MEMDESC_FLAGS_NONE = 0, MEMDESC_FLAGS_ALLOC_AS_LOCALIZED = 1;
constexpr unsigned NV_MEMORY_DEFAULT = 0, NV0050_CTRL_MEMSET_FLAGS_ASYNC = 1;
constexpr unsigned NV0050_CTRL_MEMSET_FLAGS_PIPELINED = 2;
struct Assertion {};
void Assert(bool value) { if (!value) throw Assertion{}; }
#define NV_ASSERT(value) Assert(value)
#define NV_ASSERT_OR_RETURN(value, status) do { if (!(value)) { NV_ASSERT(false); return status; } } while (0)
#define NV_ASSERT_OK_OR_RETURN(expression) \
    do { NV_STATUS result = (expression); NV_ASSERT_OR_RETURN(result == NV_OK, result); } while (0)
#define NV_ASSERT_OK_OR_GOTO(status, expression, label) \
    do { (status) = (expression); if ((status) != NV_OK) { NV_ASSERT(false); goto label; } } while (0)
#define NV_ASSERT_OR_GOTO(value, label) do { if (!(value)) { NV_ASSERT(false); goto label; } } while (0)
#define NV_CHECK_OK_OR_GOTO(status, level, expression, label) \
    do { (status) = (expression); if ((status) != NV_OK) goto label; } while (0)
#define NV_CHECK_OR_GOTO(level, value, label) do { if (!(value)) goto label; } while (0)
#define NV_CHECK_OK_OR_RETURN(level, status) do { if ((status) != NV_OK) return status; } while (0)
#define DBG_BREAKPOINT() NV_ASSERT(false)
template<typename... Args> void Print(Args...) {}
#define NV_PRINTF(level, ...) Print(__VA_ARGS__)
#define PMA_ADDR2FRAME(address, base) (((address) - (base)) >> PMA_PAGE_SHIFT)

enum class Point { None, Mutex, Alloc, Map, Describe, Memset, Progress, Timeout };
struct Metrics
{
    bool removed = false, failAlloc = false, submittedBeforeRemoval = false;
    Point point = Point::None;
    unsigned fireOnMemset = 1, nativeCalls = 0, commands = 0, traces = 0;
    unsigned creates = 0, destroys = 0, progressReads = 0, interrupts = 0, timeouts = 0;
    unsigned checks = 0, certificates = 0, stats = 0, numa = 0, blacklists = 0;
    unsigned readerLocks = 0, spinLocks = 0, mutexLocks = 0, localizationClears = 0;
    unsigned readerAcquires = 0;
    NvU64 progress = 0, nextId = 1;
    NV_STATUS nativeStatus = NV_OK, checkStatus = NV_OK;
    std::vector<std::unique_ptr<unsigned char[]>> allocations;
    std::vector<void*> live;
} m;
void Fire(Point point)
{
    if (m.point == point && (point != Point::Memset || m.nativeCalls == m.fireOnMemset))
    { m.point = Point::None; m.removed = true; }
}
bool NvKmsWindowsIsGpuSurpriseRemoved() { return m.removed; }
void NvKmsWindowsTraceMessage(const char*) { ++m.traces; }
void* portMemAllocNonPaged(size_t size)
{
    Fire(Point::Alloc);
    if (m.failAlloc) return nullptr;
    auto storage = std::make_unique<unsigned char[]>(size);
    void* pointer = storage.get();
    m.allocations.push_back(std::move(storage)); m.live.push_back(pointer);
    return pointer;
}
void portMemFree(void* pointer)
{
    if (!pointer) return;
    auto found = std::find(m.live.begin(), m.live.end(), pointer);
    ASSERT_NE(found, m.live.end());
    m.live.erase(found);
}
void portMemCopy(void* dst, size_t capacity, const void* src, size_t size)
{ EXPECT_TRUE(size <= capacity); std::memcpy(dst, src, size); }
void portMemSet(void* dst, int value, size_t size) { std::memset(dst, value, size); }
void portSyncMutexAcquire(void*) { ++m.mutexLocks; Fire(Point::Mutex); }
void portSyncMutexRelease(void*) { EXPECT_TRUE(m.mutexLocks > 0); --m.mutexLocks; }
void portSyncSpinlockAcquire(void*) { ++m.spinLocks; }
void portSyncSpinlockRelease(void*) { EXPECT_TRUE(m.spinLocks > 0); --m.spinLocks; }
void portSyncRwLockAcquireRead(void*) { ++m.readerLocks; ++m.readerAcquires; }
void portSyncRwLockReleaseRead(void*) { EXPECT_TRUE(m.readerLocks > 0); --m.readerLocks; }
unsigned portAtomicGetSize(unsigned* value) { return *value; }
void portAtomicSetSize(unsigned* value, unsigned replacement) { *value = replacement; }
struct MemoryManager { unsigned localizedMask = 3; };
struct OBJGPU { MemoryManager memory; };
#define GPU_GET_MEMORY_MANAGER(gpu) (&(gpu)->memory)
struct RMTIMEOUT {};
bool rmGpuLockIsOwner() { return true; }
bool rmDeviceGpuLockIsOwner(unsigned) { return true; }
unsigned gpuGetInstance(OBJGPU*) { return 0; }
void gpuSetTimeout(OBJGPU*, unsigned, RMTIMEOUT*, unsigned) {}
NV_STATUS gpuCheckTimeout(OBJGPU*, RMTIMEOUT*) { ++m.timeouts; Fire(Point::Timeout); return NV_ERR_TIMEOUT; }
struct MEMORY_DESCRIPTOR { unsigned localizedMask = 0; };
MEMORY_DESCRIPTOR descriptor;
NV_STATUS memdescCreate(MEMORY_DESCRIPTOR** out, OBJGPU*, NvU64, unsigned, bool, unsigned, unsigned, unsigned)
{ ++m.creates; *out = &descriptor; return NV_OK; }
void memdescDescribe(MEMORY_DESCRIPTOR*, unsigned, NvU64, NvU64) { Fire(Point::Describe); }
void memdescSetFlag(MEMORY_DESCRIPTOR*, unsigned, bool) {}
void memdescDestroy(MEMORY_DESCRIPTOR* value) { EXPECT_TRUE(value == &descriptor); ++m.destroys; }
struct MemsetParams
{
    MEMORY_DESCRIPTOR* pMemDesc = nullptr;
    NvU64 length = 0, submittedWorkId = 0;
    unsigned flags = 0;
};
using CEUTILS_MEMSET_PARAMS = MemsetParams;
using SEC2UTILS_MEMSET_PARAMS = MemsetParams;
NV_STATUS Memset(MemsetParams* params)
{
    EXPECT_TRUE(params->pMemDesc == &descriptor && params->length != 0);
    ++m.nativeCalls;
    if (m.submittedBeforeRemoval)
    {
        ++m.commands; params->submittedWorkId = m.nextId++;
        Fire(Point::Memset);
        return NV_OK;
    }
    Fire(Point::Memset);
    if (m.nativeStatus != NV_OK) return m.nativeStatus;
    if (m.removed) return NV_ERR_GPU_IS_LOST;
    ++m.commands; params->submittedWorkId = m.nextId++;
    return NV_OK;
}
NV_STATUS ceutilsMemset(void*, MemsetParams* params) { return Memset(params); }
NV_STATUS sec2utilsMemset(void*, MemsetParams* params) { return Memset(params); }
struct SCRUB_NODE { NvU64 id = 0, base = 0, size = 0; };
using PSCRUB_NODE = SCRUB_NODE*;
struct OBJMEMSCRUB
{
    OBJGPU* pGpu = nullptr;
    void* pScrubberMutex = nullptr;
    void* pCeUtils = nullptr;
    void* pSec2Utils = nullptr;
    bool bIsEngineTypeSec2 = false;
    NvU64 lastSeenIdByClient = 0, lastSubmittedWorkId = 0;
    NvLength scrubListSize = 0;
    SCRUB_NODE* pScrubList = nullptr;
};
static NvU64 _scrubCheckProgress(OBJMEMSCRUB*)
{ ++m.progressReads; Fire(Point::Progress); return m.progress; }
static void _serviceInterrupts(OBJMEMSCRUB*) { ++m.interrupts; }
struct PMA_LOCALIZATION_INFO { unsigned status = 0; NvU64 allocatedFrameCount = 0; };
struct Map
{
    std::array<unsigned, 8> frames{};
    PMA_LOCALIZATION_INFO localization;
};
void ChangeState(void* pointer, NvU64 frame, unsigned state, unsigned mask)
{
    auto& map = *static_cast<Map*>(pointer);
    EXPECT_TRUE(m.spinLocks == 1 && frame < map.frames.size());
    map.frames[frame] = (map.frames[frame] & ~mask) | (state & mask);
    Fire(Point::Map);
}
PMA_LOCALIZATION_INFO ReadLocalization(void* pointer, NvU64) { return static_cast<Map*>(pointer)->localization; }
void ChangeLocalization(void*, NvU64, unsigned, unsigned) { ++m.localizationClears; }
struct MapInfo
{
    decltype(&ChangeState) pmaMapChangeStateAttrib = ChangeState;
    decltype(&ReadLocalization) pmaMapReadLocalizationInfo = ReadLocalization;
    decltype(&ChangeLocalization) pmaMapChangeLocalizationState = ChangeLocalization;
};
struct Region { NvU64 base = 0; };
struct PMA
{
    bool bScrubOnFree = true, bNuma = false, nodeOnlined = false;
    unsigned scrubberValid = PMA_SCRUBBER_VALID;
    void* pScrubberValidLock = nullptr;
    void* pPmaLock = nullptr;
    OBJMEMSCRUB* pScrubObj = nullptr;
    MapInfo* pMapInfo = nullptr;
    std::array<void*, 1> pRegions{};
    std::array<Region*, 1> pRegDescriptors{};
    struct { NvU64 numFreeFrames = 0; } pmaStats;
    void (*pStatsUpdateCb)(void*, NvU64) = nullptr;
    void* pStatsUpdateCtx = nullptr;
};
unsigned findRegionID(PMA*, NvU64) { return 0; }
void _pmaReallocBlacklistPages(PMA*, unsigned, NvU64, NvU64) { ++m.blacklists; }
void pmaNumaFreeInternal(PMA*, NvU64*, NvU64, NvU64, unsigned) { ++m.numa; }
NV_STATUS _pmaCheckScrubbedPages(PMA*, NvU64, NvU64*, unsigned) { ++m.checks; return m.checkStatus; }
void _pmaClearScrubBit(PMA*, SCRUB_NODE*, NvU64) { ++m.certificates; }
void Stats(void*, NvU64) { ++m.stats; }

namespace GraphicsCleanup
{
constexpr NV_STATUS NV_ERR_GPU_IN_FULLCHIP_RESET = 8;
constexpr unsigned FB_CACHE_VIDEO_MEMORY = 0, FB_CACHE_EVICT = 1;
enum GR_GLOBALCTX_BUFFER { Buffer0, Buffer1, BufferCount };
#define FOR_EACH_IN_ENUM(type, value) \
    for (value = Buffer0; value != BufferCount; value = static_cast<type>(value + 1))
#define FOR_EACH_IN_ENUM_END
struct MEMORY_DESCRIPTOR { bool freed = false, destroyed = false; };
struct KernelMemorySystem {};
struct OBJGPU { KernelMemorySystem memory; };
#define GPU_GET_KERNEL_MEMORY_SYSTEM(gpu) (&(gpu)->memory)
struct GR_GLOBALCTX_BUFFERS
{
    bool bAllocated = true;
    std::array<MEMORY_DESCRIPTOR*, BufferCount> memDesc{};
    std::array<bool, BufferCount> bInitialized{true, true};
};
struct KernelGraphics
{
    struct { GR_GLOBALCTX_BUFFERS* pGlobalCtxBuffers = nullptr; } globalCtxBuffersInfo;
};
struct Metrics
{
    bool broadcast = false, removeInCache = false;
    unsigned frees = 0, destroys = 0, cacheCalls = 0;
    NV_STATUS status = NV_OK;
} g;
bool gpumgrGetBcEnabledStatus(OBJGPU*) { return g.broadcast; }
void memdescFree(MEMORY_DESCRIPTOR* desc) { EXPECT_TRUE(!desc->freed && !desc->destroyed); desc->freed = true; ++g.frees; }
void memdescDestroy(MEMORY_DESCRIPTOR* desc) { EXPECT_TRUE(desc->freed && !desc->destroyed); desc->destroyed = true; ++g.destroys; }
NV_STATUS kmemsysCacheOp_HAL(OBJGPU*, KernelMemorySystem*, void*, unsigned space, unsigned op)
{
    EXPECT_TRUE(space == FB_CACHE_VIDEO_MEMORY && op == FB_CACHE_EVICT);
    EXPECT_TRUE(g.frees != 0 && g.frees == g.destroys);
    ++g.cacheCalls;
    if (g.removeInCache) m.removed = true;
    return g.status;
}
struct Fixture
{
    OBJGPU gpu;
    KernelGraphics graphics;
    std::array<GR_GLOBALCTX_BUFFERS, 2> contexts;
    std::array<std::array<MEMORY_DESCRIPTOR, BufferCount>, 2> descriptors;
    Fixture()
    {
        m = {}; g = {};
        graphics.globalCtxBuffersInfo.pGlobalCtxBuffers = contexts.data();
        for (unsigned ctx = 0; ctx != contexts.size(); ++ctx)
            for (unsigned buf = 0; buf != BufferCount; ++buf)
                contexts[ctx].memDesc[buf] = &descriptors[ctx][buf];
    }
    void CheckReleased(unsigned ctx)
    {
        EXPECT_TRUE(!contexts[ctx].bAllocated && g.frees == BufferCount && g.destroys == BufferCount);
        for (unsigned buf = 0; buf != BufferCount; ++buf)
        {
            EXPECT_TRUE(contexts[ctx].memDesc[buf] == nullptr && !contexts[ctx].bInitialized[buf]);
            EXPECT_TRUE(descriptors[ctx][buf].destroyed);
            EXPECT_TRUE(!descriptors[1 - ctx][buf].destroyed && contexts[1 - ctx].bAllocated);
        }
    }
};
}

#include "pma_scrub_removal_production.inc"

struct Fixture
{
    OBJGPU gpu;
    std::array<SCRUB_NODE, MAX_SCRUB_ITEMS> work{};
    OBJMEMSCRUB scrubber;
    PMA pma;
    Map map;
    MapInfo info;
    Region region;
    NvU64 page = 0;
    Fixture()
    {
        m = {};
        descriptor = {};
        scrubber.pGpu = &gpu; scrubber.pScrubList = work.data();
        pma.pScrubObj = &scrubber; pma.pMapInfo = &info;
        pma.pRegions[0] = &map; pma.pRegDescriptors[0] = &region; pma.pStatsUpdateCb = Stats;
        map.frames.fill(1 | ATTRIB_EVICTING | ATTRIB_BLACKLIST);
    }
    void CheckReleased()
    {
        EXPECT_TRUE(m.readerLocks == 0 && m.spinLocks == 0 && m.mutexLocks == 0);
        EXPECT_TRUE(m.live.empty() && m.creates == m.destroys);
        EXPECT_TRUE(pma.pScrubObj == &scrubber && pma.bScrubOnFree);
    }
    void CheckInvalid()
    {
        // Invalidate capability, not scrub policy or ownership; disabling the
        // policy could let later allocations treat unscrubbed pages as clean.
        unsigned config = ~0u;
        EXPECT_TRUE(pmaQueryConfigs(&pma, &config) == NV_OK);
        EXPECT_TRUE((config & PMA_QUERY_SCRUB_ENABLED) != 0 && (config & PMA_QUERY_SCRUB_VALID) == 0);
        EXPECT_TRUE(m.certificates == 0);
    }
};
template<typename Function> void ExpectAssert(Function function)
{
    bool asserted = false;
    try { function(); } catch (const Assertion&) { asserted = true; }
    EXPECT_TRUE(asserted);
}
#if defined(NV_PMA_TEST_BASELINE) || !defined(NV_WINDOWS_IDDCX)
TEST(GraphicsCleanup, BaselineOrNativeRemovalReproducesCacheEvictionAfterDescriptorRelease)
{
    // Comparison profiles must detect the old post-removal hardware operation,
    // even though its preceding CPU descriptor cleanup already succeeded.
    using namespace GraphicsCleanup;
    {
        GraphicsCleanup::Fixture f;
        m.removed = true; g.status = NV_ERR_GPU_IS_LOST;
        ExpectAssert([&] { kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, 0); });
        EXPECT_TRUE(g.cacheCalls == 1);
        f.CheckReleased(0);
    }
}
#endif
#if !defined(NV_PMA_TEST_BASELINE)
TEST(GraphicsCleanup, ReleasesDescriptorsBeforeCacheEvictionAcrossStatuses)
{
    using namespace GraphicsCleanup;
    for (unsigned ctx : {0u, 1u})
    {
        for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IN_FULLCHIP_RESET, NV_ERR_GPU_IS_LOST, 99})
        {
            GraphicsCleanup::Fixture f;
            g.status = status;
            if (status == NV_OK || status == NV_ERR_GPU_IN_FULLCHIP_RESET)
                kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, ctx);
            else
                ExpectAssert([&] { kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, ctx); });
            EXPECT_TRUE(g.cacheCalls == 1); f.CheckReleased(ctx);
        }
    }
}

TEST(GraphicsCleanup, EmptyOrUnallocatedContextsNeedNoCleanup)
{
    using namespace GraphicsCleanup;
    for (unsigned mode : {0u, 1u, 2u})
    {
        GraphicsCleanup::Fixture f;
        if (mode == 0) f.graphics.globalCtxBuffersInfo.pGlobalCtxBuffers = nullptr;
        if (mode == 1) f.contexts[0].bAllocated = false;
        if (mode == 2) f.contexts[0].memDesc.fill(nullptr);
        kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, 0);
        EXPECT_TRUE(g.frees == 0 && g.destroys == 0 && g.cacheCalls == 0);
    }
}

TEST(GraphicsCleanup, BroadcastModeRejectsCleanupBeforeFreeing)
{
    using namespace GraphicsCleanup;
    {
        GraphicsCleanup::Fixture f;
        g.broadcast = true; m.removed = true;
        ExpectAssert([&] { kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, 0); });
        EXPECT_TRUE(g.frees == 0 && g.cacheCalls == 0);
    }
}
#if defined(NV_WINDOWS_IDDCX)
TEST(GraphicsCleanup, RemovedGpuReleasesDescriptorsWithoutCacheEviction)
{
    // Removal skips only the absent-hardware operation, not local ownership cleanup.
    using namespace GraphicsCleanup;
    {
        GraphicsCleanup::Fixture f;
        m.removed = true;
        kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, 1);
        EXPECT_TRUE(g.cacheCalls == 0 && m.traces != 0); f.CheckReleased(1);
    }
}

TEST(GraphicsCleanup, RemovalInsideCacheEvictionHonorsStatusPolicy)
{
    // A disconnect can race the call; GPU-lost becomes expected, unrelated
    // errors still assert after the descriptors have been released.
    using namespace GraphicsCleanup;
    for (NV_STATUS status : {NV_OK, NV_ERR_GPU_IN_FULLCHIP_RESET, NV_ERR_GPU_IS_LOST, 99})
    {
        GraphicsCleanup::Fixture f;
        g.status = status; g.removeInCache = true;
        if (status == 99) ExpectAssert([&] { kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, 0); });
        else kgraphicsFreeGlobalCtxBuffers_IMPL(&f.gpu, &f.graphics, 0);
        EXPECT_TRUE(g.cacheCalls == 1); f.CheckReleased(0);
        if (status == NV_ERR_GPU_IS_LOST) EXPECT_TRUE(m.traces != 0);
    }
}
#endif
#endif
#if defined(NV_PMA_TEST_BASELINE)
TEST(PmaScrubRemoval, BaselineFreeReproducesSubmissionAfterRemovalAndGpuLostAssertion)
{
    // The pinned source must exhibit the unsafe submission and interrupted
    // cleanup; a passing reproduction is not a successful removal path.
    Fixture f;
    m.removed = true;
    ExpectAssert([&] { pmaFreePages(&f.pma, &f.page, 1, _PMA_64KB, 0); });
    EXPECT_TRUE(m.checks == 1 && m.nativeCalls == 1 && m.creates == 1 && m.destroys == 0);
    EXPECT_TRUE(f.pma.scrubberValid == PMA_SCRUBBER_VALID);
}
#else
TEST(PmaScrubRemoval, SubmitsLocalizedWorkAndAssertsUnexpectedBackendErrors)
{
    for (bool sec2 : {false, true})
    {
        Fixture f;
        f.scrubber.bIsEngineTypeSec2 = sec2;
        EXPECT_TRUE(_scrubMemory(&f.scrubber, 0, _PMA_64KB, 0, 1, SCRUBBER_SUBMIT_FLAGS_LOCALIZED_SCRUB) == NV_OK);
        EXPECT_TRUE(m.commands == 1 && f.scrubber.lastSubmittedWorkId == 1 && descriptor.localizedMask == 3);
        f.CheckReleased();
        for (NV_STATUS status : {NV_ERR_GPU_IS_LOST, 99})
        {
            Fixture bad;
            bad.scrubber.bIsEngineTypeSec2 = sec2; m.nativeStatus = status;
            ExpectAssert([&] { _scrubMemory(&bad.scrubber, 0, _PMA_64KB, 0, 1, 0); });
        }
    }
}

TEST(PmaScrubRemoval, FreePreservesEvictionBlacklistAndOptionalScrubState)
{
    for (bool skip : {false, true})
    {
        Fixture f;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_128KB, skip ? PMA_FREE_SKIP_SCRUB : 0);
        EXPECT_TRUE(m.stats == 1 && m.blacklists == 1 && m.nativeCalls == (skip ? 0u : 1u));
        EXPECT_TRUE(f.pma.scrubberValid == PMA_SCRUBBER_VALID && m.certificates == 0);
        for (unsigned i = 0; i != 2; ++i)
            EXPECT_TRUE(f.map.frames[i] == (ATTRIB_EVICTING | ATTRIB_BLACKLIST | (skip ? 0 : ATTRIB_SCRUBBING)));
        f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, InvalidScrubberSkipsNativeSubmission)
{
    {
        Fixture f;
        f.pma.scrubberValid = PMA_SCRUBBER_INVALID;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_64KB, 0);
        EXPECT_TRUE(m.nativeCalls == 0 && m.checks == 0 && m.stats == 1);
        f.CheckInvalid(); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, FailedPageCheckInvalidatesScrubber)
{
    {
        Fixture f;
        m.checkStatus = 99;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_64KB, 0);
        EXPECT_TRUE(m.nativeCalls == 0 && m.checks == 1 && m.stats == 1);
        f.CheckInvalid(); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, NumaAndDisabledScrubbingBypassSubmission)
{
    for (bool numa : {false, true})
    {
        Fixture f;
        f.pma.bNuma = numa;
        if (!numa) f.pma.bScrubOnFree = false;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_64KB, 0);
        EXPECT_TRUE(m.nativeCalls == 0 && m.checks == 0 && m.numa == (numa ? 1u : 0u));
        EXPECT_TRUE(m.readerLocks == 0 && m.spinLocks == 0);
    }
}

TEST(PmaScrubRemoval, FullScrubListReturnsCompletedPagesBeforeSubmittingMore)
{
    {
        Fixture f;
        f.scrubber.scrubListSize = MAX_SCRUB_ITEMS;
        f.scrubber.lastSubmittedWorkId = MAX_SCRUB_ITEMS;
        for (unsigned i = 0; i != MAX_SCRUB_ITEMS; ++i) f.work[i] = {i + 1u, i * _PMA_64KB, _PMA_64KB};
        m.progress = MAX_SCRUB_ITEMS; m.nextId = MAX_SCRUB_ITEMS + 1;
        SCRUB_NODE* output = nullptr; NvU64 count = 0;
        EXPECT_TRUE(scrubSubmitPages(&f.scrubber, _PMA_64KB, &f.page, 1, &output, &count, 0) == NV_OK);
        EXPECT_TRUE(output != nullptr && count == 1 && output[0].id == 1);
        portMemFree(output); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, PayloadWaitHandlesEmptyIncompleteAndCompleteWork)
{
    {
        Fixture f;
        EXPECT_TRUE(_waitForPayload(&f.scrubber, 0, _PMA_64KB - 1) == NV_OK);
        f.work[0] = {1, 0, _PMA_64KB};
        f.scrubber.lastSubmittedWorkId = 1; f.scrubber.scrubListSize = 1;
        EXPECT_TRUE(_waitForPayload(&f.scrubber, 0, _PMA_64KB - 1) == NV_ERR_TIMEOUT);
        m.progress = 1;
        EXPECT_TRUE(_waitForPayload(&f.scrubber, 0, _PMA_64KB - 1) == NV_OK);
        f.CheckReleased();
    }
}
#if defined(NV_WINDOWS_IDDCX)
TEST(PmaScrubRemoval, RemovalDuringSubmissionPreservesLastSubmittedId)
{
    // Publish an ID only for a successful submission. A late disconnect keeps
    // the prior ID even if the backend already received the command.
    for (bool sec2 : {false, true})
    {
        for (Point point : {Point::None, Point::Describe, Point::Memset})
        {
            Fixture f;
            f.scrubber.bIsEngineTypeSec2 = sec2; f.scrubber.lastSubmittedWorkId = 17;
            m.removed = point == Point::None; m.point = point;
            EXPECT_TRUE(_scrubMemory(&f.scrubber, 0, _PMA_64KB, 0, 18, 0) == NV_ERR_GPU_IS_LOST);
            EXPECT_TRUE(f.scrubber.lastSubmittedWorkId == 17 && m.commands == 0);
            EXPECT_TRUE(m.nativeCalls == (point == Point::Memset ? 1u : 0u));
            f.CheckReleased();
        }
        Fixture f;
        f.scrubber.bIsEngineTypeSec2 = sec2; m.nativeStatus = 99; m.point = Point::Memset;
        ExpectAssert([&] { _scrubMemory(&f.scrubber, 0, _PMA_64KB, 0, 1, 0); });
        Fixture late;
        late.scrubber.bIsEngineTypeSec2 = sec2; late.scrubber.lastSubmittedWorkId = 17;
        m.submittedBeforeRemoval = true; m.point = Point::Memset;
        EXPECT_TRUE(_scrubMemory(&late.scrubber, 0, _PMA_64KB, 0, 18, 0) == NV_ERR_GPU_IS_LOST);
        EXPECT_TRUE(m.commands == 1 && late.scrubber.lastSubmittedWorkId == 17);
        late.CheckReleased();
    }
}

TEST(PmaScrubRemoval, RemovedFreeSkipsChecksAndSubmission)
{
    for (bool skip : {false, true})
    {
        Fixture f;
        m.removed = true;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_128KB, skip ? PMA_FREE_SKIP_SCRUB : 0);
        EXPECT_TRUE(m.nativeCalls == 0 && m.checks == 0 && m.stats == 1);
        EXPECT_TRUE(m.creates == 0 && m.progressReads == 0);
        EXPECT_TRUE(f.map.frames[0] == (ATTRIB_EVICTING | ATTRIB_BLACKLIST));
        f.CheckInvalid(); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, RemovalAtEachFreeStageClearsLocalizationWithoutCertifyingPages)
{
    for (Point point : {Point::Map, Point::Mutex, Point::Alloc, Point::Describe, Point::Memset})
    {
        Fixture f;
        m.point = point;
        f.map.localization.status = LOCALIZATION_STATE_IS_LOCALIZED;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_64KB, 0);
        EXPECT_TRUE(m.stats == 1 && m.localizationClears == 1);
        EXPECT_TRUE(m.commands == 0 && m.nativeCalls == (point == Point::Memset ? 1u : 0u));
        EXPECT_TRUE(f.scrubber.lastSubmittedWorkId == 0 && f.scrubber.scrubListSize == 0);
        f.CheckInvalid(); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, RemovalDuringProgressInvalidatesAlreadySubmittedWork)
{
    {
        Fixture f;
        m.point = Point::Progress;
        pmaFreePages(&f.pma, &f.page, 1, _PMA_64KB, 0);
        EXPECT_TRUE(m.commands == 1 && f.scrubber.lastSubmittedWorkId == 1 && f.scrubber.scrubListSize == 1);
        f.CheckInvalid(); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, FullListRemovalClearsOutputsWithoutAdvancingSubmission)
{
    for (Point point : {Point::None, Point::Progress, Point::Timeout, Point::Memset})
    {
        Fixture f;
        f.scrubber.scrubListSize = MAX_SCRUB_ITEMS;
        f.scrubber.lastSubmittedWorkId = MAX_SCRUB_ITEMS;
        m.removed = point == Point::None; m.point = point;
        if (point == Point::Memset) m.progress = MAX_SCRUB_ITEMS;
        SCRUB_NODE sentinel;
        SCRUB_NODE* output = &sentinel; NvU64 count = 99;
        EXPECT_TRUE(scrubSubmitPages(&f.scrubber, _PMA_64KB, &f.page, 1, &output, &count, 0) == NV_ERR_GPU_IS_LOST);
        EXPECT_TRUE(output == nullptr && count == 0 && m.commands == 0 && m.certificates == 0);
        EXPECT_TRUE(f.scrubber.lastSubmittedWorkId == MAX_SCRUB_ITEMS);
        f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, PartialSubmissionOnRemovalKeepsOnlyAcceptedWork)
{
    // Keep the first accepted command when the second fails, but return no
    // completed-page list or clean-page certificate from that partial batch.
    {
        Fixture f;
        NvU64 pages[]{0, 4 * _PMA_64KB};
        m.point = Point::Memset; m.fireOnMemset = 2;
        SCRUB_NODE* output = nullptr; NvU64 count = 99;
        EXPECT_TRUE(scrubSubmitPages(&f.scrubber, _PMA_64KB, pages, 2, &output, &count, 0) == NV_ERR_GPU_IS_LOST);
        EXPECT_TRUE(m.commands == 1 && f.scrubber.lastSubmittedWorkId == 1 && f.scrubber.scrubListSize == 1);
        EXPECT_TRUE(output == nullptr && count == 0); f.CheckReleased();
    }
}

TEST(PmaScrubRemoval, RemovedPayloadWaitDoesNotAdvanceClientProgress)
{
    for (Point point : {Point::None, Point::Progress, Point::Timeout})
    {
        Fixture f;
        f.work[0] = {1, 0, _PMA_64KB}; f.scrubber.scrubListSize = 1; f.scrubber.lastSubmittedWorkId = 1;
        m.removed = point == Point::None; m.point = point;
        EXPECT_TRUE(_waitForPayload(&f.scrubber, 0, _PMA_64KB - 1) == NV_ERR_GPU_IS_LOST);
        EXPECT_TRUE(f.scrubber.lastSeenIdByClient == 0 && m.certificates == 0);
        f.CheckReleased();
    }
}
#endif
#endif
