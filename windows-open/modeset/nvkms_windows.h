#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NV_KMS_WINDOWS_GPU_INFO
{
	NvU32 count;
	NvU32 gpuId;
	NvU32 domain;
	NvU8 bus;
	NvU8 slot;
	NvU8 function;
} NV_KMS_WINDOWS_GPU_INFO;

typedef struct NV_KMS_WINDOWS_DISPLAY_INFO
{
	NvU32 connectorCount;
	NvU32 displayCount;
	NvU32 connectedCount;
	NvU32 edidBytes;
	NvU8 edidChecksum;
} NV_KMS_WINDOWS_DISPLAY_INFO;

#define NV_KMS_WINDOWS_MAX_EDID_BYTES 2048
#define NV_KMS_WINDOWS_MAX_DISPLAY_MODES 256
#define NV_KMS_WINDOWS_MAX_LAYERS 8
#define NV_KMS_WINDOWS_GAMMA_ENTRIES 1024

typedef struct NV_KMS_WINDOWS_SURFACE NV_KMS_WINDOWS_SURFACE;
typedef void (*NV_KMS_WINDOWS_DISPLAY_CHANGE_CALLBACK)(void* context);
typedef NvBool (*NV_KMS_WINDOWS_GPU_REMOVED_CALLBACK)(void* context);

typedef enum NV_KMS_WINDOWS_SCANOUT_FORMAT
{
	NV_KMS_WINDOWS_SCANOUT_FORMAT_XRGB8 = 0,
	NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16 = 1,
	NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8 = 2,
} NV_KMS_WINDOWS_SCANOUT_FORMAT;

typedef enum NV_KMS_WINDOWS_COLOR_SPACE
{
	NV_KMS_WINDOWS_COLOR_SPACE_SDR = 0,
	NV_KMS_WINDOWS_COLOR_SPACE_SDR_WCG = 1,
	NV_KMS_WINDOWS_COLOR_SPACE_HDR10 = 2,
} NV_KMS_WINDOWS_COLOR_SPACE;

typedef struct NV_KMS_WINDOWS_HDR_METADATA
{
	NvU16 displayPrimaries[3][2];
	NvU16 whitePoint[2];
	NvU16 maxMasteringLuminance;
	NvU16 minMasteringLuminance;
	NvU16 maxContentLightLevel;
	NvU16 maxFrameAverageLightLevel;
} NV_KMS_WINDOWS_HDR_METADATA;

typedef struct NV_KMS_WINDOWS_COLOR_TRANSFORM
{
	NvS32 csc[3][4];
	NvU16 gammaRed[NV_KMS_WINDOWS_GAMMA_ENTRIES];
	NvU16 gammaGreen[NV_KMS_WINDOWS_GAMMA_ENTRIES];
	NvU16 gammaBlue[NV_KMS_WINDOWS_GAMMA_ENTRIES];
	NvBool cscEnabled;
	NvBool gammaEnabled;
} NV_KMS_WINDOWS_COLOR_TRANSFORM;

typedef struct NV_KMS_WINDOWS_MODE
{
	NvU32 kmsModeIndex;
	NvU32 refreshRateMillihertz;
	NvU32 pixelClockHz;
	NvU32 hVisible;
	NvU32 hSyncStart;
	NvU32 hSyncEnd;
	NvU32 hTotal;
	NvU32 hSkew;
	NvU32 vVisible;
	NvU32 vSyncStart;
	NvU32 vSyncEnd;
	NvU32 vTotal;
	NvU32 widthMillimeters;
	NvU32 heightMillimeters;
	NvBool rgb8Supported;
	NvBool rgb10Supported;
	NvBool interlaced;
	NvBool doubleScan;
	NvBool hSyncPos;
	NvBool hSyncNeg;
	NvBool vSyncPos;
	NvBool vSyncNeg;
	NvBool preferred;
} NV_KMS_WINDOWS_MODE;

typedef struct NV_KMS_WINDOWS_DISPLAY_STATE
{
	NvU32 displayHandle;
	NvU32 connectorType;
	NvU32 connectorIndex;
	NvU32 edidBytes;
	NvU8 edid[NV_KMS_WINDOWS_MAX_EDID_BYTES];
	NvU32 modeCount;
	NvU32 preferredModeIndex;
	NV_KMS_WINDOWS_MODE modes[NV_KMS_WINDOWS_MAX_DISPLAY_MODES];
	NvU32 headCount;
	NvU32 maxLayersPerHead;
	NvU32 maxCursorSize;
	NvBool supportsMultiLayerFlip;
	NvBool vrrSupported;
} NV_KMS_WINDOWS_DISPLAY_STATE;

typedef struct NV_KMS_WINDOWS_LAYER
{
	NV_KMS_WINDOWS_SURFACE* surface;
	NvU32 sourceX;
	NvU32 sourceY;
	NvU32 sourceWidth;
	NvU32 sourceHeight;
	NvS32 destinationX;
	NvS32 destinationY;
	NvU32 destinationWidth;
	NvU32 destinationHeight;
	NvU8 opacity;
	NV_KMS_WINDOWS_COLOR_SPACE colorSpace;
	NV_KMS_WINDOWS_HDR_METADATA hdrMetadata;
	NvBool hdrMetadataEnabled;
	const NV_KMS_WINDOWS_COLOR_TRANSFORM* colorTransform;
	NvBool useDirtyBands;
	NvU32 dirtyBandCount;
	struct {
		NvU32 top;
		NvU32 bottom;
	} dirtyBands[2];
	NvBool presentationPolicySpecified;
	NvBool immediateNonTearing;
	/* Per-flip permission; enabling VRR support itself requires a modeset. */
	NvBool vrrEnabled;
	NvBool skipUpload;
	NV_KMS_WINDOWS_SURFACE* uploadSource;
	NvU32 rotationDegrees;
	NvBool reflectX;
	NvBool pixelAlpha;
	NV_KMS_WINDOWS_SURFACE* inputLut;
	/* Requires skipUpload, no uploadSource, and the identical current surface.
	 * Unchanged configs do not flip; metadata edits re-submit it because KAPI
	 * gates RR/composition and completion notifiers on surfaceChanged. */
	NvBool reuseSurface;
} NV_KMS_WINDOWS_LAYER;

typedef struct NV_KMS_WINDOWS_LAYER_CAPS
{
	NvU32 rotationMask;
	NvBool reflectionSupported;
	NvBool pixelAlphaSupported;
	NvBool inputLutSupported;
	NvU32 minWidth;
	NvU32 maxWidth;
	NvU32 minHeight;
	NvU32 maxHeight;
	/* Format bits use NV_KMS_WINDOWS_SCANOUT_FORMAT values. */
	NvU32 formatMask;
	/* Exact joint rotation/reflection support; bit = rotation/90 + reflectX*4. */
	NvU32 transformMask;
	NvBool pixelAlphaWithOpacitySupported;
	/* KAPI CSC/gain plumbing; ValidateLayers checks the actual candidate. */
	NvBool cscSupported;
	NvBool gainOverrideSupported;
} NV_KMS_WINDOWS_LAYER_CAPS;

typedef struct NV_KMS_WINDOWS_MODESET_OUTPUT
{
	NvU32 displayHandle;
	NV_KMS_WINDOWS_MODE mode;
	NV_KMS_WINDOWS_SURFACE* surface;
	NvU8 colorBpc;
	NV_KMS_WINDOWS_COLOR_SPACE colorSpace;
	NV_KMS_WINDOWS_HDR_METADATA hdrMetadata;
	NvBool hdrMetadataEnabled;
	NvU32 assignedHead;
	NvBool vrrPreference;
	NvBool vrrEnabled;
	NvBool immediateNonTearing;
} NV_KMS_WINDOWS_MODESET_OUTPUT;

typedef struct NV_KMS_WINDOWS_PRESENT_TIMING
{
	NvU64 lockTicks;
	NvU64 uploadTicks;
	NvU64 prepareTicks;
	NvU64 submitTicks;
	NvU64 pendingTicks;
	NvU64 uploadBytes;
	NvU32 uploadRegions;
	NvBool vrrFlip;
	NvU64 uploadStart;
	NvU64 uploadEnd;
} NV_KMS_WINDOWS_PRESENT_TIMING;

typedef struct NV_KMS_WINDOWS_CURSOR_TIMING
{
	NvU64 begin;
	NvU64 lockTicks;
	NvU64 uploadTicks;
	NvU64 programStart;
	NvU64 programEnd;
} NV_KMS_WINDOWS_CURSOR_TIMING;

// The predicate must not block; its context must outlive GPU unregistration.
NV_STATUS NvKmsWindowsRegisterGpu(
	NvU32 gpuId,
	NvU32 domain,
	NvU8 bus,
	NvU8 slot,
	NvU8 function,
	void* osDevice,
	NV_KMS_WINDOWS_GPU_REMOVED_CALLBACK removedCallback,
	void* removedContext);
void NvKmsWindowsUnregisterGpu(NvU32 gpuId);
NV_STATUS NvKmsWindowsProbeGpuEnumeration(NV_KMS_WINDOWS_GPU_INFO* gpuInfo);
NV_STATUS NvKmsWindowsProbeDisplays(NV_KMS_WINDOWS_DISPLAY_INFO* displayInfo);
NV_STATUS NvKmsWindowsGetConnectedDisplayCount(NvU32* displayCount);
NV_STATUS NvKmsWindowsGetDisplay(
	NvU32 connectedDisplayIndex,
	NV_KMS_WINDOWS_DISPLAY_STATE* displayState);
NV_STATUS NvKmsWindowsGetPrimaryDisplay(
	NV_KMS_WINDOWS_DISPLAY_STATE* displayState);
NV_STATUS NvKmsWindowsMonitorI2c(
	NvU32 displayHandle,
	NvU32 transmitAddress,
	NvU32 transmitSize,
	const NvU8* transmit,
	NvU32 receiveAddress,
	NvU32 receiveSize,
	NvU8* receive,
	NvU32 receiveFlags);
NV_STATUS NvKmsWindowsCreateScanoutSurface(
	NvU32 width,
	NvU32 height,
	NV_KMS_WINDOWS_SCANOUT_FORMAT format,
	NV_KMS_WINDOWS_SURFACE** surface,
	void** pixels,
	NvU32* pitch,
	NvBool honorPixelCachePolicy);
void NvKmsWindowsDestroyScanoutSurface(NV_KMS_WINDOWS_SURFACE* surface);
NV_STATUS NvKmsWindowsGetScanoutBuffer(
	NV_KMS_WINDOWS_SURFACE* surface,
	void** pixels,
	NvU64* allocationSize);
NV_STATUS NvKmsWindowsSetDisplayModes(
	NV_KMS_WINDOWS_MODESET_OUTPUT* outputs,
	NvU32 outputCount);
/* Validate the current cached mode/layers with a different VRR preference.
 * Set validates again then applies a same-head modeset without uploads,
 * preserving current surfaces/LUTs/cursor. Unsupported enable requests reject;
 * callers update their actual VrrEnabled only after successful Set, and drain
 * every previously enabled layer before reclaiming any lease. Set checks the
 * actual NVKMS reply and restores the last-good mode on a policy mismatch;
 * drain layers even after this rejection. INVALID_STATE on rollback failure
 * requires stopping submissions and retaining leases until recovery. */
NV_STATUS NvKmsWindowsValidateVrrPreference(
	NvU32 head, NvU32 displayHandle, NvBool vrrPreference);
NV_STATUS NvKmsWindowsSetVrrPreference(
	NvU32 head, NvU32 displayHandle, NvBool vrrPreference,
	NvBool* vrrEnabled);
NV_STATUS NvKmsWindowsPresentLayers(
	NvU32 head,
	NvU32 displayHandle,
	const NV_KMS_WINDOWS_LAYER* layers,
	NvU32 layerCount);
/* All head layers participate atomically: omitted layers are disabled and
 * retain their own pending/timestamp notifier for independent retirement. */
NV_STATUS NvKmsWindowsPresentLayersTimed(
	NvU32 head,
	NvU32 displayHandle,
	const NV_KMS_WINDOWS_LAYER* layers,
	NvU32 layerCount,
	NvBool preserveFrontBuffer,
	NV_KMS_WINDOWS_PRESENT_TIMING* timing);
NV_STATUS NvKmsWindowsGetLayerCaps(
	NvU32 head, NvU32 layer, NV_KMS_WINDOWS_LAYER_CAPS* caps);
/* Test-only NVKMS validation: no uploads, allocations or presentation-cache changes.
 * Matching, already-cached legacy head gamma is accepted. New/changed legacy
 * head gamma returns NOT_SUPPORTED because its OLUT has not been prepared;
 * an inputLut is a separate per-layer stage, not an equivalent head-gamma fix. */
NV_STATUS NvKmsWindowsValidateLayers(
	NvU32 head, NvU32 displayHandle,
	const NV_KMS_WINDOWS_LAYER* layers, NvU32 layerCount);
/* Immutable, caller-owned lease. Destroy with DestroyScanoutSurface only after
 * every layer using it has switched away AND drained. CSC/gain is supplied
 * through colorTransform.csc: signed S3.16, diagonal 0x10000 is unit gain. */
NV_STATUS NvKmsWindowsCreateInputLut(
	const NV_KMS_WINDOWS_COLOR_TRANSFORM* transform,
	NV_KMS_WINDOWS_SURFACE** surface);
NV_STATUS NvKmsWindowsUploadLayer(
	const NV_KMS_WINDOWS_LAYER* layer,
	NV_KMS_WINDOWS_PRESENT_TIMING* timing);
NV_STATUS NvKmsWindowsGetFlipPending(NvU32 head, NvBool* pending);
NV_STATUS NvKmsWindowsGetFlipTimestamp(NvU32 head, NvU64* timestamp);
NV_STATUS NvKmsWindowsGetLayerFlipPending(
	NvU32 head, NvU32 layer, NvBool* pending);
NV_STATUS NvKmsWindowsGetLayerFlipTimestamp(
	NvU32 head, NvU32 layer, NvU64* timestamp);
NV_STATUS NvKmsWindowsTraceFlipChannels(NvU32 head);
NV_STATUS NvKmsWindowsUpdateCursor(
	NvU32 head,
	NvU32 displayHandle,
	NV_KMS_WINDOWS_SURFACE* surface,
	NvBool surfaceChanged,
	NvS32 x,
	NvS32 y,
	NvU32 sdrWhiteLevel);
NV_STATUS NvKmsWindowsUpdateCursorTimed(
	NvU32 head,
	NvU32 displayHandle,
	NV_KMS_WINDOWS_SURFACE* surface,
	NvBool surfaceChanged,
	NvS32 x,
	NvS32 y,
	NvU32 sdrWhiteLevel,
	NV_KMS_WINDOWS_CURSOR_TIMING* timing);
NV_STATUS NvKmsWindowsProbeRmClient(NvHandle* clientHandle);
void NvKmsWindowsSetDisplayChangeCallback(
	NV_KMS_WINDOWS_DISPLAY_CHANGE_CALLBACK callback,
	void* context);
NV_STATUS NvKmsWindowsInitialize(void);
void NvKmsWindowsShutdown(void);
NvBool NvKmsWindowsIsGpuRemoved(void);
// Unavailability alone is not evidence of physical surprise removal.
NvBool NvKmsWindowsIsGpuSurpriseRemoved(void);

#ifdef __cplusplus
}
#endif
