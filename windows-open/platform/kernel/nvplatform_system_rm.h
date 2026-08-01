#pragma once

#include <stdarg.h>

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
NV_STATUS NvPlatformRmGetSmbiosTable(
	void** table,
	NvU64* tableSize,
	NvU64* structureCount,
	NvU32* version);
void NvPlatformRmPutSmbiosTable(void* table);
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
NV_STATUS NvPlatformRmAddCrashLog(void* buffer, NvU32 length);
void NvPlatformRmDeleteCrashLog(void* buffer);
void NvPlatformRmLogErrorV(
	NvU32 xid,
	const char* format,
	va_list arguments);
void NvPlatformRmCaptureAssertion(void);
__declspec(noreturn) void NvPlatformRmBugCheck(NvU32 bugCode);

typedef void NvPlatformRmSystemWorkFunction(void* context);

// Owns the WDK work-item queue used by RM callbacks and drains it at unload.
NV_STATUS NvPlatformRmSystemWorkInitialize(void);

NV_STATUS NvPlatformRmSemaAcquire(void);

NV_STATUS NvPlatformRmSemaTryAcquire(void);

void NvPlatformRmSemaRelease(void);

NV_STATUS NvPlatformRmQueueSystemWork(
	NvPlatformRmSystemWorkFunction* function,
	void* context);

NvBool NvPlatformRmSystemWorkIsActive(void);

void NvPlatformRmDrainSystemWork(void);

// Prevents queued GPU callbacks from resolving an OBJGPU while RM deletes it.
// RM locks must be acquired before entering this gate; GPU destroy already
// holds the API lock when it closes and drains the gate.
NvBool NvPlatformRmGpuWorkAcquire(void);

void NvPlatformRmGpuWorkRelease(void);

void NvPlatformRmGpuDestroyBegin(void);

void NvPlatformRmGpuDestroyEnd(void);

#ifdef __cplusplus
}
#endif