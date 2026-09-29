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

NV_STATUS NvKmsWindowsRegisterGpu(
	NvU32 gpuId,
	NvU32 domain,
	NvU8 bus,
	NvU8 slot,
	NvU8 function,
	void* osDevice);
void NvKmsWindowsUnregisterGpu(NvU32 gpuId);
NV_STATUS NvKmsWindowsProbeGpuEnumeration(NV_KMS_WINDOWS_GPU_INFO* gpuInfo);
NV_STATUS NvKmsWindowsProbeRmClient(NvHandle* clientHandle);

#ifdef __cplusplus
}
#endif
