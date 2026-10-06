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
} NV_KMS_WINDOWS_LAYER;

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
} NV_KMS_WINDOWS_PRESENT_TIMING;

NV_STATUS NvKmsWindowsRegisterGpu(
	NvU32 gpuId,
	NvU32 domain,
	NvU8 bus,
	NvU8 slot,
	NvU8 function,
	void* osDevice);
void NvKmsWindowsUnregisterGpu(NvU32 gpuId);
NV_STATUS NvKmsWindowsProbeGpuEnumeration(NV_KMS_WINDOWS_GPU_INFO* gpuInfo);
NV_STATUS NvKmsWindowsProbeDisplays(NV_KMS_WINDOWS_DISPLAY_INFO* displayInfo);
NV_STATUS NvKmsWindowsGetConnectedDisplayCount(NvU32* displayCount);
NV_STATUS NvKmsWindowsGetDisplay(
	NvU32 connectedDisplayIndex,
	NV_KMS_WINDOWS_DISPLAY_STATE* displayState);
NV_STATUS NvKmsWindowsGetPrimaryDisplay(
	NV_KMS_WINDOWS_DISPLAY_STATE* displayState);
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
NV_STATUS NvKmsWindowsPresentLayers(
	NvU32 head,
	NvU32 displayHandle,
	const NV_KMS_WINDOWS_LAYER* layers,
	NvU32 layerCount);
NV_STATUS NvKmsWindowsPresentLayersTimed(
	NvU32 head,
	NvU32 displayHandle,
	const NV_KMS_WINDOWS_LAYER* layers,
	NvU32 layerCount,
	NvBool preserveFrontBuffer,
	NV_KMS_WINDOWS_PRESENT_TIMING* timing);
NV_STATUS NvKmsWindowsUpdateCursor(
	NvU32 head,
	NvU32 displayHandle,
	NV_KMS_WINDOWS_SURFACE* surface,
	NvBool surfaceChanged,
	NvS32 x,
	NvS32 y,
	NvU32 sdrWhiteLevel);
NV_STATUS NvKmsWindowsProbeRmClient(NvHandle* clientHandle);
void NvKmsWindowsSetDisplayChangeCallback(
	NV_KMS_WINDOWS_DISPLAY_CHANGE_CALLBACK callback,
	void* context);
NV_STATUS NvKmsWindowsInitialize(void);
void NvKmsWindowsShutdown(void);

#ifdef __cplusplus
}
#endif
