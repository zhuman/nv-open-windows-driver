#define NVKMS_WINDOWS_REQUEST_TEST
#include "..\nvkms_windows.cpp"
#include <gtest/gtest.h>

#include <cstdarg>
#include <string>
#include <vector>
#include "class/clc37e.h"

struct TestFlipState
{
    NvKmsKapiSurface* pSurfaceEvo[2] = {};
    struct { NvU32 width, height; } sizeIn = {}, sizeOut = {};
    struct { struct { NvKmsKapiSurface* pSurfaceEvo; } surface = {}; }
        completionNotifier;
    struct
    {
        NvBool usingSyncpt = NV_FALSE;
        struct
        {
            struct
            {
                struct { NvKmsKapiSurface* pSurfaceEvo; }
                    acquireSurface = {}, releaseSurface = {};
            } semaphores;
        } u;
    } syncObject;
};
#include "flip_channel_validation.hpp"

namespace {
bool captureChannelTrace = false;
NV_PLATFORM_RM_GPU_ATTACH_INFO snapshotAttach = {};
NV_STATUS snapshotStatus = NV_OK;
unsigned snapshotQueries = 0;
std::vector<NvU64> registerReads;
std::vector<std::string> channelMessages;
}

extern "C" void NvKmsWindowsTraceMessage(const char* message)
{
    if (captureChannelTrace) channelMessages.emplace_back(message);
}
extern "C" NV_STATUS NvPlatformRmGpuGetAttachInfo(
    const NV_PLATFORM_RM_GPU_CONTEXT*, NV_PLATFORM_RM_GPU_ATTACH_INFO* info)
{
    ++snapshotQueries;
    *info = snapshotAttach;
    return snapshotStatus;
}
extern "C" NvU32 NvPlatformRmReadRegister32(const volatile void* address)
{
    const auto offset = static_cast<const volatile NvU8*>(address) -
        static_cast<const volatile NvU8*>(snapshotAttach.apertures[0].kernelAddress);
    if (offset < 0 || static_cast<NvU64>(offset) + sizeof(NvU32) >
        snapshotAttach.apertures[0].length)
    {
        ADD_FAILURE() << "Display register read exceeds the supplied aperture";
        return 0;
    }
    registerReads.push_back(static_cast<NvU64>(offset));
    return 0xAC050707;
}
extern "C" NvU32 nvRmApiFree(NvU32, NvU32, NvU32) { return NV_OK; }
extern "C" void nvFreeUnixRmHandleInternal(NVUnixRmHandleAllocatorPtr, NvU32) {}
extern "C" void nvkms_sema_down(nvkms_sema_handle_t*) {}
extern "C" void nvkms_sema_up(nvkms_sema_handle_t*) {}
extern "C" int nvkms_snprintf(char* output, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int count = std::vsnprintf(output, size, format, args);
    va_end(args);
    return count;
}

namespace {

NvKmsKapiDevice testDevice = {};
NvKmsKapiDeviceResourcesInfo testResources = {};
NvKmsKapiRequestedModeSetConfig captured = {};
NvBool commitRequested = NV_FALSE;
NvBool acceptRequest = NV_TRUE;
NvBool pendingLayers[NVKMS_KAPI_LAYER_MAX] = {};
NvU32 queriedLayer = 0;
NvU32 copies = 0;
NvU32 allocations = 0;
NvU32 applies = 0;
NvU32 outputLutCalls = 0;
size_t nextSurfaceId = 0x1000;
NvBool displayConnected = NV_TRUE;
NvBool vrrSupported = NV_TRUE;
NvBool omitVrrEnable = NV_FALSE;
NvKmsKapiSurface testDisabledSentinel = {};
NV_KMS_WINDOWS_SURFACE identityLut = {};


struct TestMemory { void* pixels; };

NvBool Resources(NvKmsKapiDevice*, NvKmsKapiDeviceResourcesInfo* resources)
{
    *resources = testResources;
    return NV_TRUE;
}

NvBool DynamicDisplay(NvKmsKapiDevice*, NvKmsKapiDynamicDisplayParams* display)
{
    display->connected = displayConnected;
    display->vrrSupported = vrrSupported;
    return NV_TRUE;
}

NvBool Pending(const NvKmsKapiDevice*, NvU32, NvU32 layer, NvBool* pending)
{
    queriedLayer = layer;
    *pending = pendingLayers[layer];
    return NV_TRUE;
}

NvBool Timestamp(const NvKmsKapiDevice*, NvU32, NvU32 layer, NvU64* timestamp)
{
    queriedLayer = layer;
    *timestamp = 1000 + layer;
    return NV_TRUE;
}

NvBool Apply(NvKmsKapiDevice* device,
    const NvKmsKapiRequestedModeSetConfig* request,
    NvKmsKapiModeSetReplyConfig* reply, NvBool commit)
{
    captured = *request;
    commitRequested = commit;
    ++applies;
    for (NvU32 layer = 0; layer < device->numLayers[0]; ++layer)
    {
        const auto& config = request->headRequestedConfig[0].layerRequestedConfig[layer];
        if (!config.flags.surfaceChanged && !request->headRequestedConfig[0].flags.modeChanged)
            continue;
        TestFlipState state;
        state.pSurfaceEvo[0] = config.config.surface == &testDisabledSentinel ?
            nullptr : config.config.surface;
        state.sizeIn = {config.config.srcWidth, config.config.srcHeight};
        state.sizeOut = {config.config.dstWidth, config.config.dstHeight};
        if (commit && config.config.surface != nullptr)
            state.completionNotifier.surface.pSurfaceEvo =
                reinterpret_cast<NvKmsKapiSurface*>(1);
        if (!ValidateFlipChannelEvoHwState(&state))
        {
            reply->flipResult = NV_KMS_FLIP_RESULT_INVALID_PARAMS;
            return NV_FALSE;
        }
    }
    if (commit)
    {
        // The backend advances overlay indices even without a surface flip.
        for (NvU32 layer = 0; layer < device->numLayers[0]; ++layer)
            if (layer != 0 || request->headRequestedConfig[0].
                    layerRequestedConfig[layer].flags.surfaceChanged)
                ++device->layerState[0][layer].currFlipNotifierIndex;
    }
    reply->flipResult = acceptRequest ? NV_KMS_FLIP_RESULT_SUCCESS :
        NV_KMS_FLIP_RESULT_IN_PROGRESS;
    if (commit && acceptRequest && request->headRequestedConfig[0].flags.modeChanged)
    {
        auto params = std::make_unique<NvKmsSetModeParams>();
        params->request.commit = NV_TRUE;
        params->request.deviceHandle = device->hKmsDevice;
        params->request.requestedDispsBitMask = 1U << device->dispIdx;
        auto& disp = params->request.disp[device->dispIdx];
        disp.requestedHeadsBitMask = request->headsMask;
        for (NvU32 head = 0; head < device->numHeads; ++head)
        {
            disp.head[head].allowGsync = NV_TRUE;
            disp.head[head].allowAdaptiveSync = NVKMS_ALLOW_ADAPTIVE_SYNC_ALL;
            disp.head[head].flip.allowVrr = request->headRequestedConfig[head].modeSetConfig.vrrEnabled;
        }
        OverrideVrrModeSetPolicy(params.get());
        params->reply.status = NVKMS_SET_MODE_STATUS_SUCCESS;
        for (NvU32 head = 0; head < device->numHeads; ++head)
            params->reply.disp[device->dispIdx].head[head].vrrEnabled =
                !omitVrrEnable && (disp.head[head].allowGsync ||
                    disp.head[head].allowAdaptiveSync != NVKMS_ALLOW_ADAPTIVE_SYNC_DISABLED);
        CaptureVrrModeSetReply(params.get(), device->dispIdx, NV_TRUE);
    }
    return acceptRequest;
}

NvKmsKapiMemory* Allocate(NvKmsKapiDevice*, NvKmsKapiAllocateMemoryParams* params)
{
    ++allocations;
    auto* memory = new TestMemory{
        VirtualAlloc(nullptr, static_cast<SIZE_T>(params->size),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    if (memory->pixels == nullptr)
    {
        ADD_FAILURE() << "Unable to allocate fake scanout memory";
        delete memory;
        return nullptr;
    }
    return reinterpret_cast<NvKmsKapiMemory*>(memory);
}

void Free(NvKmsKapiDevice*, NvKmsKapiMemory* memory)
{
    auto* value = reinterpret_cast<TestMemory*>(memory);
    VirtualFree(value->pixels, 0, MEM_RELEASE);
    delete value;
}

NvBool Map(const NvKmsKapiDevice*, const NvKmsKapiMemory* memory,
    NvKmsKapiMappingType, void** pixels)
{
    *pixels = reinterpret_cast<const TestMemory*>(memory)->pixels;
    return NV_TRUE;
}

void Unmap(const NvKmsKapiDevice*, const NvKmsKapiMemory*, NvKmsKapiMappingType, const void*) {}

NvBool Copy(NvKmsKapiDevice*, NvKmsKapiMemory* destination,
    const NvKmsKapiMemory* source, NvU64 offset, NvU64 size)
{
    ++copies;
    if (destination != nullptr && source != nullptr)
        std::memcpy(static_cast<NvU8*>(reinterpret_cast<TestMemory*>(destination)->pixels) + offset,
            static_cast<const NvU8*>(reinterpret_cast<const TestMemory*>(source)->pixels) + offset,
            static_cast<size_t>(size));
    return NV_TRUE;
}

NvKmsKapiSurface* Create(NvKmsKapiDevice*, NvKmsKapiCreateSurfaceParams*)
{
    return reinterpret_cast<NvKmsKapiSurface*>(++nextSurfaceId);
}

void Destroy(NvKmsKapiDevice*, NvKmsKapiSurface*) {}

NV_KMS_WINDOWS_SURFACE Surface(size_t id, NV_KMS_WINDOWS_SCANOUT_FORMAT format)
{
    NV_KMS_WINDOWS_SURFACE surface = {};
    surface.surface = reinterpret_cast<NvKmsKapiSurface*>(id);
    surface.width = 1920;
    surface.height = 1080;
    surface.pitch = 7680;
    surface.size = static_cast<NvU64>(surface.pitch) * surface.height;
    surface.format = format;
    return surface;
}

NV_KMS_WINDOWS_LAYER Layer(NV_KMS_WINDOWS_SURFACE* surface)
{
    NV_KMS_WINDOWS_LAYER layer = {};
    layer.surface = surface;
    layer.sourceWidth = surface->width;
    layer.sourceHeight = surface->height;
    layer.destinationWidth = layer.sourceWidth;
    layer.destinationHeight = layer.sourceHeight;
    layer.opacity = 255;
    layer.skipUpload = NV_TRUE;
    return layer;
}

void InitializeMpoBackend()
{
    kmsDevice = &testDevice;
    testDevice.numHeads = 2;
    testDevice.numLayers[0] = 3;
    testDevice.numLayers[1] = 1;
    testResources.numHeads = 2;
    testResources.numLayers[0] = 3;
    testResources.numLayers[1] = 1;
    testResources.caps.maxWidthInPixels = 16384;
    testResources.caps.maxHeightInPixels = 16384;
    testResources.caps.layer[0].validCompositionModes =
        NVBIT(NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE);
    for (NvU32 index = 0; index < 3; ++index)
    {
        testResources.caps.layer[index].validRRTransforms = 0x55;
        if (index != 0)
            testResources.caps.layer[index].validCompositionModes =
                NVBIT(NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE) |
                NVBIT(NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA) |
                NVBIT(NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_SURFACE_ALPHA);
        testResources.supportedSurfaceMemoryFormats[index] =
            NVBIT64(NvKmsSurfaceMemoryFormatX8R8G8B8) |
            NVBIT64(NvKmsSurfaceMemoryFormatRF16GF16BF16AF16) |
            NVBIT64(NvKmsSurfaceMemoryFormatA8R8G8B8);
        testResources.lutCaps.layer[index].ilut.supported = NV_TRUE;
        testResources.lutCaps.layer[index].ilut.lutEntries = LutEntries;
        testResources.lutCaps.layer[index].ilut.entryFormat = NVKMS_LUT_FORMAT_FP16;
    }
    kmsFunctions.getDeviceResourcesInfo = &Resources;
    kmsFunctions.getDynamicDisplayInfo = &DynamicDisplay;
    kmsFunctions.getFlipPendingStatus = &Pending;
    kmsFunctions.getFlipTimestamp = &Timestamp;
    kmsFunctions.applyModeSetConfig = &Apply;
    kmsFunctions.allocateMemory = &Allocate;
    kmsFunctions.freeMemory = &Free;
    kmsFunctions.mapMemory = &Map;
    kmsFunctions.unmapMemory = &Unmap;
    kmsFunctions.copyMemoryRange = &Copy;
    kmsFunctions.createSurface = &Create;
    kmsFunctions.destroySurface = &Destroy;
    identityLut.surface = reinterpret_cast<NvKmsKapiSurface*>(0x100);
    kmsDefaultLutSurface = &identityLut;
}

class MpoRequests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        testDevice = {};
        testResources = {};
        captured = {};
        commitRequested = NV_FALSE;
        acceptRequest = displayConnected = vrrSupported = NV_TRUE;
        omitVrrEnable = NV_FALSE;
        std::memset(pendingLayers, 0, sizeof(pendingLayers));
        queriedLayer = copies = allocations = applies = outputLutCalls = 0;
        nextSurfaceId = 0x1000;
        testDisabledSentinel = {};
        identityLut = {};
        kmsFunctions = {};
        std::memset(gammaCache, 0, sizeof(gammaCache));
        std::memset(layerCache, 0, sizeof(layerCache));
        std::memset(retiringSurfaces, 0, sizeof(retiringSurfaces));
        std::memset(layerDisablePending, 0, sizeof(layerDisablePending));
        std::memset(modeSetCache, 0, sizeof(modeSetCache));
        std::memset(modeSetCacheValid, 0, sizeof(modeSetCacheValid));
        std::memset(cursorCache, 0, sizeof(cursorCache));
        std::memset(kmsGammaSurfaces, 0, sizeof(kmsGammaSurfaces));
        vrrModeSetPolicy = {};
        gpuRegistered = NV_FALSE;
        captureChannelTrace = false;
        snapshotAttach = {};
        snapshotStatus = NV_OK;
        snapshotQueries = 0;
        registerReads.clear();
        channelMessages.clear();
        InitializeMpoBackend();
    }
    void TearDown() override
    {
        kmsDevice = nullptr;
        kmsDefaultLutSurface = nullptr;
        gpuRegistered = NV_FALSE;
        captureChannelTrace = false;
    }
};

TEST_F(MpoRequests, ReportsCapsAndPerLayerQueries)
{
    // The fake supports 0/180-degree rotations and reflections, not all quarter
    // turns. Overlay queries must still select that layer's own notifier.
    NV_KMS_WINDOWS_LAYER_CAPS caps = {};
    EXPECT_TRUE(NvKmsWindowsGetLayerCaps(0, 1, &caps) == NV_OK);
    EXPECT_TRUE(caps.rotationMask == 5 && caps.transformMask == 0x55 && caps.reflectionSupported);
    EXPECT_TRUE(caps.pixelAlphaSupported && caps.pixelAlphaWithOpacitySupported && caps.inputLutSupported);
    EXPECT_TRUE(caps.formatMask == 7 && caps.cscSupported && caps.gainOverrideSupported);
    EXPECT_TRUE(NvKmsWindowsGetLayerCaps(1, 1, &caps) == NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(NvKmsWindowsGetLayerCaps(0, 3, &caps) == NV_ERR_INVALID_ARGUMENT);
    NvBool pending = NV_FALSE;
    NvU64 timestamp = 0;
    EXPECT_TRUE(NvKmsWindowsGetLayerFlipPending(0, 2, &pending) == NV_OK && queriedLayer == 2);
    EXPECT_TRUE(NvKmsWindowsGetLayerFlipTimestamp(0, 2, &timestamp) == NV_OK && timestamp == 1002);
    EXPECT_TRUE(NvKmsWindowsGetFlipPending(0, &pending) == NV_OK && queriedLayer == 0);
    EXPECT_TRUE(NvKmsWindowsGetFlipTimestamp(0, &timestamp) == NV_OK && timestamp == 1000);
    EXPECT_TRUE(NvKmsWindowsGetLayerFlipPending(1, 1, &pending) == NV_ERR_INVALID_ARGUMENT);
}

TEST_F(MpoRequests, BuildsRequestsWithReuseRetirementAndCompatibilityChecks)
{
    auto primary = Surface(1, NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16);
    auto overlay = Surface(2, NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8);
    NV_KMS_WINDOWS_LAYER layers[] = {Layer(&primary), Layer(&overlay)};
    // Test-only validation builds the request without committing, uploading,
    // or changing the live layer cache.
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 1) == NV_OK);
    EXPECT_TRUE(!commitRequested && !primary.presented && copies == 0 && allocations == 0);
    const auto& oldPrimary = captured.headRequestedConfig[0].layerRequestedConfig[0];
    EXPECT_TRUE(oldPrimary.flags.surfaceChanged && oldPrimary.config.rrParams.rotation == NVKMS_ROTATION_0);
    EXPECT_TRUE(oldPrimary.config.compParams.compMode == NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE &&
        oldPrimary.config.compParams.surfaceAlpha == 255);
    EXPECT_TRUE(oldPrimary.config.minPresentInterval == 1 && layerCache[0][0].surface == nullptr);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_OK);
    EXPECT_TRUE(commitRequested && primary.presented);
    layers[0].reuseSurface = NV_TRUE;
    const NvU32 submissions = applies;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 1) == NV_OK);
    EXPECT_TRUE(applies == submissions + 1 && !captured.headRequestedConfig[0].
        layerRequestedConfig[0].flags.surfaceChanged);
    pendingLayers[0] = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 1) == NV_OK);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_ERR_IN_USE);
    pendingLayers[0] = NV_FALSE;
    layers[0].skipUpload = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 1) == NV_ERR_INVALID_ARGUMENT);
    layers[0].skipUpload = NV_TRUE;
    layers[0].uploadSource = &overlay;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 1) == NV_ERR_INVALID_ARGUMENT);
    layers[0].uploadSource = nullptr;
    layers[1].opacity = 160;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].config.compParams.compMode ==
        NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_SURFACE_ALPHA);
    layers[1].pixelAlpha = NV_TRUE;
    layers[1].opacity = 160;
    layers[1].rotationDegrees = 180;
    layers[1].reflectX = NV_TRUE;
    layers[1].sourceWidth = 800;
    layers[1].sourceHeight = 600;
    layers[1].destinationWidth = 400;
    layers[1].destinationHeight = 300;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    const auto& request = captured.headRequestedConfig[0].layerRequestedConfig[1];
    EXPECT_TRUE(request.config.compParams.compMode == NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_SURFACE_ALPHA &&
        request.config.compParams.surfaceAlpha == 160);
    EXPECT_TRUE(request.config.rrParams.rotation == NVKMS_ROTATION_180 && request.config.rrParams.reflectionX);
    EXPECT_TRUE(request.config.srcWidth == 800 && request.config.dstWidth == 400);
    EXPECT_TRUE(request.config.ilut.enabled &&
        request.config.ilut.lutSurface == identityLut.surface &&
        request.config.ilut.offset == offsetof(LutData, base) &&
        request.config.ilut.lutEntries == LutEntries);
    // An inactive plane's notifier may be uninitialized after the modeset.
    pendingLayers[1] = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr) == NV_OK);
    pendingLayers[1] = NV_FALSE;
    const auto cursorNotifier = testDevice.layerState[0][1].currFlipNotifierIndex;
    EXPECT_TRUE(NvKmsWindowsUpdateCursor(0, 7, nullptr, NV_FALSE, 20, 30, 80) == NV_OK);
    EXPECT_TRUE(testDevice.layerState[0][1].currFlipNotifierIndex == cursorNotifier);
    layers[1].reuseSurface = NV_TRUE;
    const auto oldNotifier = testDevice.layerState[0][1].currFlipNotifierIndex;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr) == NV_OK);
    EXPECT_TRUE(!captured.headRequestedConfig[0].layerRequestedConfig[1].flags.surfaceChanged);
    EXPECT_TRUE(testDevice.layerState[0][1].currFlipNotifierIndex == oldNotifier);
    layers[1].opacity = 255;
    // Reusing pixels does not make a changed composition a notifier-free no-op.
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].flags.surfaceChanged);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].config.compParams.compMode ==
        NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].config.compParams.surfaceAlpha == 0);
    layers[1].rotationDegrees = 90;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_NOT_SUPPORTED);
    layers[1].rotationDegrees = 13;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_INVALID_ARGUMENT);
    layers[1].rotationDegrees = 180;
    layers[1].sourceX = 1900;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_INVALID_ARGUMENT);
    layers[1].sourceX = 0;
    layers[1].sourceX = 32;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_NOT_SUPPORTED);
    layers[1].sourceX = 0;
    layers[0].pixelAlpha = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_NOT_SUPPORTED);
    layers[0].pixelAlpha = NV_FALSE;
    NV_KMS_WINDOWS_PRESENT_TIMING timing = {};
    EXPECT_TRUE(NvKmsWindowsUploadLayer(&layers[0], &timing) == NV_ERR_IN_USE);
    layers[0].reuseSurface = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_ERR_INVALID_STATE);
    auto nextPrimary = Surface(3, NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16);
    layers[0] = Layer(&nextPrimary);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr) == NV_OK);
    pendingLayers[0] = NV_TRUE;
    const auto retired = Layer(&primary);
    EXPECT_TRUE(NvKmsWindowsUploadLayer(&retired, &timing) == NV_ERR_IN_USE);
    pendingLayers[0] = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsUploadLayer(&retired, &timing) == NV_OK);
    layers[0].reuseSurface = NV_TRUE;
    layers[0].destinationX = 10;
    acceptRequest = NV_FALSE;
    const auto before = testDevice.layerState[0][0].currFlipNotifierIndex;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr) == NV_ERR_IN_USE);
    EXPECT_TRUE(testDevice.layerState[0][0].currFlipNotifierIndex == before);
    EXPECT_TRUE(layerCache[0][0].dstX == 0);
    acceptRequest = NV_TRUE;
    layers[0].destinationX = 0;
    layers[0].presentationPolicySpecified = NV_TRUE;
    layers[0].immediateNonTearing = NV_TRUE;
    layers[0].vrrEnabled = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].modeSetConfig.vrrEnabled &&
        captured.headRequestedConfig[0].layerRequestedConfig[0].config.minPresentInterval == 0 &&
        captured.headRequestedConfig[0].layerRequestedConfig[0].flags.surfaceChanged);
    layers[0].presentationPolicySpecified = NV_FALSE;
    layers[0].immediateNonTearing = NV_FALSE;
    layers[0].vrrEnabled = NV_FALSE;
    pendingLayers[1] = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_ERR_IN_USE);
    pendingLayers[1] = NV_FALSE;
    const auto overlayNotifier = testDevice.layerState[0][1].currFlipNotifierIndex;
    // Overlay disable has no independent completion: atomically resubmit the
    // primary and retain the overlay lease until that primary update retires.
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 1) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].config.surface == nullptr);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].flags.surfaceChanged &&
        captured.headRequestedConfig[0].layerRequestedConfig[1].config.surface == nullptr);
    EXPECT_TRUE(layerCache[0][1].surface == nullptr &&
        testDevice.layerState[0][1].currFlipNotifierIndex == overlayNotifier);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[0].flags.surfaceChanged &&
        !captured.headRequestedConfig[0].layerRequestedConfig[0].config.tearing);
    pendingLayers[0] = NV_TRUE;
    NvBool stillPending = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsGetLayerFlipPending(0, 1, &stillPending) == NV_OK && stillPending &&
        queriedLayer == 0);
    EXPECT_TRUE(layerDisablePending[0][1] && retiringSurfaces[0][1] == overlay.surface);
    EXPECT_TRUE(NvKmsWindowsUploadLayer(&retired, &timing) == NV_ERR_IN_USE);
    EXPECT_TRUE(NvKmsWindowsUploadLayer(&layers[1], &timing) == NV_ERR_IN_USE);
    EXPECT_TRUE(CheckSurfaceWritable(overlay.surface) == NV_ERR_IN_USE);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_ERR_IN_USE);
    pendingLayers[0] = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsGetLayerFlipPending(0, 1, &stillPending) == NV_OK && !stillPending);
    EXPECT_TRUE(!layerDisablePending[0][1] && retiringSurfaces[0][1] == nullptr);
    EXPECT_TRUE(CheckSurfaceWritable(overlay.surface) == NV_OK);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 1, NV_TRUE, nullptr) == NV_OK);
    EXPECT_TRUE(testDevice.layerState[0][1].currFlipNotifierIndex == overlayNotifier);
    pendingLayers[1] = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsGetLayerFlipPending(0, 1, &stillPending) == NV_OK && stillPending);
    EXPECT_TRUE(NvKmsWindowsGetFlipPending(0, &stillPending) == NV_OK && !stillPending);
    pendingLayers[1] = NV_FALSE;
    pendingLayers[0] = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, nullptr, 0, NV_TRUE, nullptr) == NV_ERR_IN_USE);
    pendingLayers[0] = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, nullptr, 0, NV_TRUE, nullptr) ==
        NV_ERR_NOT_SUPPORTED);
}

TEST_F(MpoRequests, RejectsCompletionNotifierOnDisabledSentinel)
{
    // Validation can accept the sentinel, but committing it with a notifier
    // is invalid; a null surface is the supported layer-disable request.
    NvKmsKapiRequestedModeSetConfig request = {};
    auto& config = request.headRequestedConfig[0].layerRequestedConfig[1];
    config.flags.surfaceChanged = NV_TRUE;
    config.config.surface = &testDisabledSentinel;
    NvKmsKapiModeSetReplyConfig reply = {};
    EXPECT_TRUE(Apply(&testDevice, &request, &reply, NV_FALSE));
    EXPECT_TRUE(!Apply(&testDevice, &request, &reply, NV_TRUE) &&
        reply.flipResult == NV_KMS_FLIP_RESULT_INVALID_PARAMS);
    config.config.surface = nullptr;
    EXPECT_TRUE(Apply(&testDevice, &request, &reply, NV_TRUE) &&
        reply.flipResult == NV_KMS_FLIP_RESULT_SUCCESS);
}

TEST_F(MpoRequests, UploadsImmutableInputLutAndTransportsCscWithoutOutputGamma)
{
    // Per-plane LUT/CSC must not silently become head-wide output gamma.
    // Validation reuses uploaded immutable storage rather than allocating more.
    NV_KMS_WINDOWS_COLOR_TRANSFORM transform = {};
    transform.gammaEnabled = NV_TRUE;
    for (NvU32 index = 0; index < NV_KMS_WINDOWS_GAMMA_ENTRIES; ++index)
    {
        transform.gammaRed[index] = static_cast<NvU16>((index * 65535U + 511U) / 1023U);
        transform.gammaGreen[index] = 65535;
        transform.gammaBlue[index] = 0;
    }
    transform.gammaRed[511] = 0x1234;
    NV_KMS_WINDOWS_SURFACE* lut = nullptr;
    ASSERT_EQ(NvKmsWindowsCreateInputLut(&transform, &lut), NV_OK);
    ASSERT_NE(lut, nullptr);
    EXPECT_TRUE(lut->immutableInputLut && outputLutCalls == 0);
    EXPECT_TRUE(copies == 1 && allocations == 2);
    const auto* data = static_cast<const LutData*>(lut->pixels);
    EXPECT_TRUE(data->base[4].Red == 0 && data->base[4].Green == 0x3c00 && data->base[4].Blue == 0);
    EXPECT_TRUE(data->base[1027].Red == 0x3c00 && data->base[1028].Red == 0x3c00);
    // Endpoint-only checks could miss an incorrectly converted or uploaded ramp.
    EXPECT_TRUE(data->base[4 + 511].Red == FloatToFp16(0x1234 / 65535.0F));
    EXPECT_TRUE(FloatToFp16(1.0F / 65535.0F) == 0x100);
    void* pixels = nullptr;
    NvU64 size = 0;
    EXPECT_TRUE(NvKmsWindowsGetScanoutBuffer(lut, &pixels, &size) == NV_ERR_INVALID_ARGUMENT);
    auto primary = Surface(4, NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16);
    auto overlay = Surface(5, NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8);
    NV_KMS_WINDOWS_LAYER layers[] = {Layer(&primary), Layer(&overlay)};
    transform.cscEnabled = NV_TRUE;
    transform.csc[0][0] = transform.csc[1][1] = transform.csc[2][2] = 0x18000;
    layers[1].colorTransform = &transform;
    layers[1].inputLut = lut;
    const NvU32 allocated = allocations;
    const NvU32 copied = copies;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(allocations == allocated && copies == copied && outputLutCalls == 0);
    const auto& config = captured.headRequestedConfig[0].layerRequestedConfig[1];
    EXPECT_TRUE(config.flags.ilutChanged && config.config.ilut.lutSurface == lut->surface &&
        config.config.ilut.offset == offsetof(LutData, base) &&
        config.config.ilut.lutEntries == 1025 && config.config.ilut.vssSegments == 0);
    EXPECT_TRUE(config.flags.cscChanged && config.config.csc.m[0][0] == 0x18000);
    layers[1].colorSpace = NV_KMS_WINDOWS_COLOR_SPACE_HDR10;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[1].config.
        matrixOverrides.enabled.fmtCtm);
    testResources.lutCaps.layer[1].ilut.supported = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_NOT_SUPPORTED);
    testResources.lutCaps.layer[1].ilut.supported = NV_TRUE;
    layers[1].inputLut = nullptr;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_NOT_SUPPORTED);
    layers[1].colorTransform = nullptr;
    layers[0].colorTransform = &transform;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_NOT_SUPPORTED);
    layers[0].inputLut = lut;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(allocations == allocated && copies == copied && !gammaCache[0].valid);
    layers[1].inputLut = lut;
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr) == NV_OK);
    EXPECT_TRUE(layerCache[0][0].ilut.lutSurface == lut->surface &&
        layerCache[0][1].ilut.lutSurface == lut->surface);
    layers[0].reuseSurface = layers[1].reuseSurface = NV_TRUE;
    layers[0].inputLut = layers[1].inputLut = nullptr;
    transform.gammaEnabled = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_OK);
    EXPECT_TRUE(captured.headRequestedConfig[0].layerRequestedConfig[0].flags.ilutChanged &&
        !captured.headRequestedConfig[0].layerRequestedConfig[0].config.ilut.enabled);
    EXPECT_TRUE(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr) == NV_OK);
    EXPECT_TRUE(layerCache[0][0].ilut.lutSurface == nullptr &&
        layerCache[0][1].ilut.lutSurface == identityLut.surface);
    EXPECT_TRUE(allocations == allocated && copies == copied);
    auto* const savedIdentity = kmsDefaultLutSurface;
    kmsDefaultLutSurface = nullptr;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, layers, 2) == NV_ERR_INVALID_STATE);
    kmsDefaultLutSurface = savedIdentity;
    NvKmsWindowsDestroyScanoutSurface(lut);
}

TEST_F(MpoRequests, ReconfiguresVrrTransactionallyWithoutChangingSurfaces)
{
    // VRR changes are same-head modesets: preserve scanout/cursor/HDR/LUT state
    // and report the actual reply, not just the requested preference.
    auto primarySurface = Surface(4, NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16);
    auto overlaySurface = Surface(5, NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8);
    NV_KMS_WINDOWS_LAYER layers[] = {Layer(&primarySurface), Layer(&overlaySurface)};
    ASSERT_EQ(NvKmsWindowsPresentLayersTimed(0, 7, layers, 2, NV_TRUE, nullptr), NV_OK);
    modeSetCacheValid[0] = NV_TRUE;
    auto& mode = modeSetCache[0];
    mode.bActive = NV_TRUE;
    mode.numDisplays = 1;
    mode.displays[0] = 7;
    mode.mode.timings.hVisible = 1920;
    mode.mode.timings.vVisible = 1080;
    mode.mode.timings.refreshRate = 60000;
    mode.hdrInfoFrame.enabled = NV_TRUE;
    mode.hdrInfoFrame.eotf = NVKMS_INFOFRAME_EOTF_ST2084;
    mode.olut.enabled = NV_TRUE;
    mode.olut.lutSurface = reinterpret_cast<NvKmsKapiSurface*>(0x2222);
    const auto primary = layerCache[0][0].surface;
    const auto overlay = layerCache[0][1].surface;
    auto cursor = Surface(7, NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8);
    cursor.width = cursor.height = 64;
    EXPECT_TRUE(NvKmsWindowsUpdateCursor(0, 7, &cursor, NV_TRUE, 25, 35, 80) == NV_OK);
    const auto copied = copies;
    const auto allocated = allocations;
    EXPECT_TRUE(NvKmsWindowsValidateVrrPreference(0, 7, NV_TRUE) == NV_OK);
    EXPECT_TRUE(!commitRequested && captured.headRequestedConfig[0].flags.modeChanged);
    const auto& request = captured.headRequestedConfig[0];
    EXPECT_TRUE(request.modeSetConfig.mode.timings.hVisible == 1920 &&
        request.modeSetConfig.hdrInfoFrame.enabled && request.modeSetConfig.olut.lutSurface ==
            mode.olut.lutSurface);
    EXPECT_TRUE(request.layerRequestedConfig[0].config.surface == primary &&
        request.layerRequestedConfig[1].config.surface == overlay);
    EXPECT_TRUE(request.cursorRequestedConfig.surface == cursor.surface &&
        request.cursorRequestedConfig.dstX == 25 && request.cursorRequestedConfig.dstY == 35);
    EXPECT_TRUE(!mode.vrrEnabled && copies == copied && allocations == allocated);
    NvBool enabled = NV_FALSE;
    pendingLayers[1] = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, &enabled) == NV_ERR_IN_USE);
    EXPECT_TRUE(!enabled && !mode.vrrEnabled);
    pendingLayers[1] = NV_FALSE;
    const auto submitted = applies;
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, &enabled) == NV_OK);
    EXPECT_TRUE(applies == submitted + 2 && commitRequested && enabled && mode.vrrEnabled);
    EXPECT_TRUE(layerCache[0][0].surface == primary && layerCache[0][1].surface == overlay &&
        copies == copied && allocations == allocated);
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_FALSE, &enabled) == NV_OK);
    EXPECT_TRUE(!enabled && !mode.vrrEnabled);
    omitVrrEnable = NV_TRUE;
    // A commit that omits the requested VRR bit must trigger rollback, not success.
    const auto beforeOmission = applies;
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, &enabled) == NV_ERR_NOT_SUPPORTED);
    EXPECT_TRUE(!enabled && !mode.vrrEnabled && applies == beforeOmission + 4);
    EXPECT_TRUE(!captured.headRequestedConfig[0].modeSetConfig.vrrEnabled &&
        layerCache[0][0].surface == primary && layerCache[0][1].surface == overlay);
    mode.vrrEnabled = enabled = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, &enabled) == NV_ERR_INVALID_STATE);
    EXPECT_TRUE(enabled && mode.vrrEnabled);
    mode.vrrEnabled = enabled = NV_FALSE;
    omitVrrEnable = NV_FALSE;
    vrrSupported = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, &enabled) == NV_ERR_NOT_SUPPORTED);
    EXPECT_TRUE(!enabled && !mode.vrrEnabled);
    vrrSupported = NV_TRUE;
    acceptRequest = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, &enabled) == NV_ERR_IN_USE);
    EXPECT_TRUE(!enabled && !mode.vrrEnabled);
    acceptRequest = NV_TRUE;
    displayConnected = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsValidateVrrPreference(0, 7, NV_TRUE) == NV_ERR_OBJECT_NOT_FOUND);
    displayConnected = NV_TRUE;
    EXPECT_TRUE(NvKmsWindowsValidateVrrPreference(0, 8, NV_TRUE) == NV_ERR_INVALID_STATE);
    EXPECT_TRUE(NvKmsWindowsSetVrrPreference(0, 7, NV_TRUE, nullptr) == NV_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(NvKmsWindowsValidateVrrPreference(1, 7, NV_TRUE) == NV_ERR_INVALID_STATE);
    modeSetCacheValid[0] = NV_FALSE;
}

TEST_F(MpoRequests, VrrPolicyOverrideIsScopedToRequestedDeviceAndHead)
{
    // A temporary override must not affect another head/device or outlive its guard.
    auto params = std::make_unique<NvKmsSetModeParams>();
    params->request.deviceHandle = testDevice.hKmsDevice;
    params->request.requestedDispsBitMask = 1;
    params->request.disp[0].requestedHeadsBitMask = 3;
    params->request.disp[0].head[1].allowAdaptiveSync = NVKMS_ALLOW_ADAPTIVE_SYNC_DISABLED;
    {
        VrrModeSetPolicyGuard guard(0, NV_TRUE);
        OverrideVrrModeSetPolicy(params.get());
        EXPECT_TRUE(params->request.disp[0].head[0].allowGsync &&
            params->request.disp[0].head[0].allowAdaptiveSync == NVKMS_ALLOW_ADAPTIVE_SYNC_ALL &&
            params->request.disp[0].head[0].flip.allowVrr);
        EXPECT_TRUE(!params->request.disp[0].head[1].allowGsync &&
            params->request.disp[0].head[1].allowAdaptiveSync == NVKMS_ALLOW_ADAPTIVE_SYNC_DISABLED);
        CaptureVrrModeSetReply(params.get(), 0, NV_TRUE);
        EXPECT_TRUE(!vrrModeSetPolicy.replyValid);
    }
    {
        VrrModeSetPolicyGuard guard(0, NV_FALSE);
        OverrideVrrModeSetPolicy(params.get());
        EXPECT_TRUE(!params->request.disp[0].head[0].allowGsync &&
            params->request.disp[0].head[0].allowAdaptiveSync == NVKMS_ALLOW_ADAPTIVE_SYNC_DISABLED &&
            !params->request.disp[0].head[0].flip.allowVrr);
        ++params->request.deviceHandle;
        params->request.disp[0].head[0].allowGsync = NV_TRUE;
        OverrideVrrModeSetPolicy(params.get());
        EXPECT_TRUE(params->request.disp[0].head[0].allowGsync);
    }
    EXPECT_TRUE(!vrrModeSetPolicy.overridePolicy);
}

TEST_F(MpoRequests, ExistingGammaValidationDoesNotAllocateOrUpload)
{
    // A matching programmed gamma may be reused, never created or changed
    // as a hidden side effect of test-only validation.
    auto surface = Surface(8, NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16);
    auto layer = Layer(&surface);
    NV_KMS_WINDOWS_COLOR_TRANSFORM transform = {};
    transform.gammaEnabled = NV_TRUE;
    for (NvU32 index = 0; index < NV_KMS_WINDOWS_GAMMA_ENTRIES; ++index)
        transform.gammaRed[index] = transform.gammaGreen[index] =
            transform.gammaBlue[index] = static_cast<NvU16>(index * 65535U / 1023U);
    gammaCache[0].valid = NV_TRUE;
    std::memcpy(gammaCache[0].red, transform.gammaRed, sizeof(transform.gammaRed));
    std::memcpy(gammaCache[0].green, transform.gammaGreen, sizeof(transform.gammaGreen));
    std::memcpy(gammaCache[0].blue, transform.gammaBlue, sizeof(transform.gammaBlue));
    layer.colorSpace = NV_KMS_WINDOWS_COLOR_SPACE_HDR10;
    layer.colorTransform = &transform;
    const auto copied = copies;
    const auto allocated = allocations;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, &layer, 1) == NV_OK);
    EXPECT_TRUE(!commitRequested && gammaCache[0].valid && copies == copied && allocations == allocated);
    ++transform.gammaRed[128];
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, &layer, 1) == NV_ERR_NOT_SUPPORTED);
    --transform.gammaRed[128];
    gammaCache[0].valid = NV_FALSE;
    EXPECT_TRUE(NvKmsWindowsValidateLayers(0, 7, &layer, 1) == NV_ERR_NOT_SUPPORTED);
    EXPECT_TRUE(copies == copied && allocations == allocated);
}

TEST_F(MpoRequests, ChannelSnapshotsReadOnlySupportedRegistersWithinAperture)
{
    // Diagnostics stay observational even with a truncated aperture; reject
    // unsupported class/instance pairs before reading any registers.
    const auto aperture = std::make_unique<NvU8[]>(0x6B0000);
    snapshotAttach.apertureCount = 1;
    snapshotAttach.apertures[0].kernelAddress = aperture.get();
    snapshotAttach.apertures[0].length = 0x6B0000;
    gpuRegistered = true;
    captureChannelTrace = true;
    auto trace = [](NvU32 hwclass, NvU32 instance) {
        registerReads.clear();
        channelMessages.clear();
        snapshotQueries = 0;
        NvKmsWindowsTraceDisplayChannelRegisters(hwclass, instance);
    };
    trace(NVC77D_CORE_CHANNEL_DMA, 0);
    EXPECT_TRUE(snapshotQueries == 1 && registerReads.size() == 2);
    EXPECT_TRUE(registerReads[0] == NV_PDISP_FE_CHNCTL_CORE);
    EXPECT_TRUE(registerReads[1] == NV_PDISP_FE_CHNSTATUS_CORE);
    EXPECT_TRUE(channelMessages[0].find("update=") == std::string::npos);

    const NvU32 methods[] = {
        NVC67E_SET_CONTEXT_DMA_ISO(0), NVC67E_SET_OFFSET(0),
        NVC67E_SET_CONTEXT_DMA_NOTIFIER, NVC67E_SET_NOTIFIER_CONTROL,
        NVC67E_SET_SIZE, NVC67E_SET_PRESENT_CONTROL,
        NVC67E_SET_COMPOSITION_CONTROL, NVC67E_SET_INTERLOCK_FLAGS,
        NVC67E_SET_WINDOW_INTERLOCK_FLAGS,
        NVC67E_SET_SIZE_IN, NVC67E_SET_SIZE_OUT,
        NVC67E_SET_STORAGE, NVC67E_SET_PLANAR_STORAGE(0),
        NVC67E_SET_PARAMS, NVC67E_SET_CONTROL_INPUT_SCALER,
        NVC67E_SET_ILUT_CONTROL, NVC67E_SET_CONTEXT_DMA_ILUT,
        NVC67E_SET_OFFSET_ILUT, NVC67E_SET_SCAN_DIRECTION,
    };
    for (NvU32 window : {6U, 7U, 31U})
    {
        // Assembly/armed banks have different strides for window and core methods.
        trace(NVC67E_WINDOW_CHANNEL_DMA, window);
        EXPECT_TRUE(snapshotQueries == 1 && registerReads.size() == 2 + 2 * ARRAYSIZE(methods) + 8);
        EXPECT_TRUE(registerReads[0] == NV_PDISP_FE_CHNCTL_WIN(window));
        EXPECT_TRUE(registerReads[1] == NV_PDISP_FE_CHNSTATUS_WIN(window));
        EXPECT_TRUE(channelMessages[0].find("update=7") != std::string::npos);
        for (unsigned index = 0; index < ARRAYSIZE(methods); ++index)
        {
            const auto assembly = NV_UDISP_FE_CHN_ASSY_BASEADR_WIN(window) + methods[index];
            EXPECT_TRUE(registerReads[2 + 2 * index] == assembly);
            EXPECT_TRUE(registerReads[3 + 2 * index] == assembly + 2048);
        }
        const NvU32 coreMethods[] = {
            NVC77D_WINDOW_SET_CONTROL(window),
            NVC77D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(window),
            NVC77D_WINDOW_SET_MAX_INPUT_SCALE_FACTOR(window),
            NVC77D_WINDOW_SET_WINDOW_USAGE_BOUNDS(window),
        };
        for (unsigned index = 0; index < ARRAYSIZE(coreMethods); ++index)
        {
            const unsigned read = 2 + 2 * ARRAYSIZE(methods) + 2 * index;
            const auto assembly = NV_UDISP_FE_CHN_ASSY_BASEADR_CORE + coreMethods[index];
            EXPECT_TRUE(registerReads[read] == assembly);
            EXPECT_TRUE(registerReads[read + 1] == assembly + 32768);
        }
    }
    trace(NVC67B_WINDOW_IMM_CHANNEL_DMA, 7);
    EXPECT_TRUE(snapshotQueries == 1 && registerReads.size() == 2);
    EXPECT_TRUE(registerReads[0] == NV_PDISP_FE_CHNCTL_WINIM(7));
    EXPECT_TRUE(registerReads[1] == NV_PDISP_FE_CHNSTATUS_WINIM(7));
    EXPECT_TRUE(channelMessages[0].find("update=") == std::string::npos);

    for (auto unsupported : {
        std::pair<NvU32, NvU32>{NVC77D_CORE_CHANNEL_DMA, 1},
        {NVC67E_WINDOW_CHANNEL_DMA, 32}, {NVC67B_WINDOW_IMM_CHANNEL_DMA, 32},
        {NVC37E_WINDOW_CHANNEL_DMA, 0}})
    {
        trace(unsupported.first, unsupported.second);
        EXPECT_TRUE(snapshotQueries == 0 && registerReads.empty());
        EXPECT_TRUE(channelMessages.size() == 1);
    }
    snapshotAttach.apertureCount = 0;
    trace(NVC67E_WINDOW_CHANNEL_DMA, 7);
    EXPECT_TRUE(registerReads.empty() && channelMessages.size() == 1);
    snapshotAttach.apertureCount = 1;
    snapshotAttach.apertures[0].length = NV_PDISP_FE_CHNSTATUS_WIN(7) + 3;
    trace(NVC67E_WINDOW_CHANNEL_DMA, 7);
    EXPECT_TRUE(registerReads.empty() && channelMessages.size() == 1);
    snapshotAttach.apertures[0].length = NV_PDISP_FE_CHNSTATUS_WIN(7) + 4;
    trace(NVC67E_WINDOW_CHANNEL_DMA, 7);
    EXPECT_TRUE(registerReads.size() == 2 && channelMessages.size() == 2);
    snapshotAttach.apertures[0].length = 0x6B0000;
    snapshotStatus = NV_ERR_INVALID_STATE;
    trace(NVC67E_WINDOW_CHANNEL_DMA, 7);
    EXPECT_TRUE(registerReads.empty() && channelMessages.size() == 1);
    snapshotStatus = NV_OK;
    gpuRegistered = false;
    trace(NVC67E_WINDOW_CHANNEL_DMA, 7);
    EXPECT_TRUE(snapshotQueries == 0 && registerReads.empty() && channelMessages.size() == 1);
    captureChannelTrace = false;
}

} // namespace

extern "C" NvBool nvEvoGetPqOutputLutC5(void*, NvU32, NvU32*, NvU32*)
{
    ++outputLutCalls;
    return NV_FALSE;
}
