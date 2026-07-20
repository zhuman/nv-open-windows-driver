#pragma once

#include "nvstatus.h"
#include "nvtypes.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformRmGetRandomBytes(NvU8* bytes, NvU32 byteCount);

void* NvPlatformRmCreateEcPublicKey(
	NvU32 curveNid,
	const NvU8* publicKey,
	NvU32 publicKeyLength);

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

NvBool NvPlatformRmVerifyRsaPkcs1(
	void* context,
	NvU32 hashNid,
	const NvU8* hash,
	NvU32 hashLength,
	const NvU8* signature,
	NvU32 signatureLength);

bool libspdm_sha256_hash_all(
	const void* data,
	size_t dataLength,
	NvU8* digest);

bool libspdm_sha384_hash_all(
	const void* data,
	size_t dataLength,
	NvU8* digest);

bool libspdm_ecdsa_verify(
	void* context,
	size_t hashNid,
	const NvU8* hash,
	size_t hashLength,
	const NvU8* signature,
	size_t signatureLength);

void libspdm_ec_free(void* context);

#ifdef __cplusplus
}
#endif