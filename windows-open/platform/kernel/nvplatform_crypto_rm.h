#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmGetRandomBytes(NvU8* bytes, NvU32 byteCount);

void* NvPlatformRmCreateRsaPublicKey(
	const NvU8* modulus,
	NvU32 modulusLength,
	const NvU8* exponent,
	NvU32 exponentLength);

void NvPlatformRmDestroyRsaKey(void* context);

NvBool NvPlatformRmVerifyRsaPss(
	void* context,
	NvU32 hashNid,
	const NvU8* hash,
	NvU32 hashLength,
	const NvU8* signature,
	NvU32 signatureLength);

#ifdef __cplusplus
}
#endif