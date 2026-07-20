#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NvBool NvPlatformRmIsNotebook(void);
NvBool NvPlatformRmIsDebuggerPresent(void);
NvBool NvPlatformRmIsCurrentSubjectAdministrator(void);
NV_STATUS NvPlatformRmGetFirmwareVariable(
	const char* name,
	const void* vendorGuid,
	NvU8* data,
	NvU32* dataLength);
NV_STATUS NvPlatformRmGetAcpiTable(
	NvU32 tableSignature,
	void* table,
	NvU32 tableSize,
	NvU32* returnSize);
NV_STATUS NvPlatformRmGetDriverImageInfo(
	void** imageBase,
	NvU8 debugIdentifier[16],
	NvU32* debugAge,
	NvU32* codeOffset);
NV_STATUS NvPlatformRmGetOsVersion(
	NvU32* majorVersion,
	NvU32* minorVersion,
	NvU32* buildNumber,
	NvU32* productType);
void NvPlatformRmCaptureAssertion(void);
__declspec(noreturn) void NvPlatformRmBugCheck(NvU32 bugCode);

typedef void NvPlatformRmSystemWorkFunction(void* context);

NV_STATUS NvPlatformRmSystemWorkInitialize(void);

NV_STATUS NvPlatformRmSemaAcquire(void);

NV_STATUS NvPlatformRmSemaTryAcquire(void);

void NvPlatformRmSemaRelease(void);

NV_STATUS NvPlatformRmQueueSystemWork(
	NvPlatformRmSystemWorkFunction* function,
	void* context);

void NvPlatformRmDrainSystemWork(void);

#ifdef __cplusplus
}
#endif