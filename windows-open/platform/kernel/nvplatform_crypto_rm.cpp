#if defined(NV_USER_MODE)
#include <windows.h>
#else
#include <ntddk.h>
#endif
#include <bcrypt.h>

#include "nvplatform_crypto_rm.h"

#if defined(NV_USER_MODE)
#pragma comment(lib, "bcrypt.lib")
#define ExAllocatePool2(flags, size, tag) \
    ((void)(flags), (void)(tag), HeapAlloc(GetProcessHeap(), 0, (size)))
#define ExFreePoolWithTag(allocation, tag) \
    ((void)(tag), HeapFree(GetProcessHeap(), 0, (allocation)))
#define POOL_FLAG_NON_PAGED 0
#define PASSIVE_LEVEL 0
#define KeGetCurrentIrql() PASSIVE_LEVEL
#define NT_SUCCESS(status) (static_cast<NTSTATUS>(status) >= 0)
#define STATUS_INSUFFICIENT_RESOURCES static_cast<NTSTATUS>(0xC000009AL)
#define MAXULONG 0xffffffffUL
#else
#pragma comment(lib, "cng.lib")
#endif

namespace {

constexpr ULONG CryptoPoolTag = 'rCvN';

struct HashContext
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
    PUCHAR object;
    ULONG objectLength;
    ULONG digestLength;
};

struct AesGcmContext
{
    BCRYPT_ALG_HANDLE algorithm;
};

struct EcContext
{
    ULONG coordinateLength;
    ULONG publicDataLength;
    NvU8 publicData[2 * 48];
    ULONG privateBlobLength;
    NvU8 privateBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 3 * 48];
};

struct RsaContext
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_KEY_HANDLE key;
    ULONG modulusLength;
};

EcContext* CreateEcContext(size_t nid) noexcept
{
    ULONG coordinateLength = 0;
    if (nid == 0x0204)
    {
        coordinateLength = 32;
    }
    else if (nid == 0x0205)
    {
        coordinateLength = 48;
    }
    else
    {
        return nullptr;
    }

    auto* context = static_cast<EcContext*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(EcContext),
        CryptoPoolTag));
    if (context != nullptr)
    {
        RtlZeroMemory(context, sizeof(*context));
        context->coordinateLength = coordinateLength;
    }
    return context;
}

void DestroyEcContext(EcContext* context) noexcept
{
    if (context != nullptr)
    {
        RtlSecureZeroMemory(context, sizeof(*context));
        ExFreePoolWithTag(context, CryptoPoolTag);
    }
}

void DestroyHashContext(HashContext* context) noexcept
{
    if (context == nullptr)
    {
        return;
    }

    if (context->hash != nullptr)
    {
        BCryptDestroyHash(context->hash);
    }
    if (context->algorithm != nullptr)
    {
        BCryptCloseAlgorithmProvider(context->algorithm, 0);
    }
    if (context->object != nullptr)
    {
        ExFreePoolWithTag(context->object, CryptoPoolTag);
    }
    ExFreePoolWithTag(context, CryptoPoolTag);
}

bool CreateHashHandle(
    HashContext* context,
    const NvU8* key = nullptr,
    ULONG keyLength = 0) noexcept
{
    return NT_SUCCESS(BCryptCreateHash(
        context->algorithm,
        &context->hash,
        context->object,
        context->objectLength,
        const_cast<PUCHAR>(key),
        keyLength,
        0));
}

HashContext* CreateHashContext(
    PCWSTR algorithmName,
    ULONG digestLength,
    ULONG flags = 0) noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return nullptr;
    }

    auto* context = static_cast<HashContext*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(HashContext),
        CryptoPoolTag));
    if (context == nullptr)
    {
        return nullptr;
    }
    RtlZeroMemory(context, sizeof(*context));
    context->digestLength = digestLength;

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &context->algorithm,
        algorithmName,
        nullptr,
        flags);
    ULONG resultLength = 0;
    if (NT_SUCCESS(status))
    {
        status = BCryptGetProperty(
            context->algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&context->objectLength),
            sizeof(context->objectLength),
            &resultLength,
            0);
    }
    if (NT_SUCCESS(status))
    {
        context->object = static_cast<PUCHAR>(ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            context->objectLength,
            CryptoPoolTag));
        if (context->object == nullptr)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (!NT_SUCCESS(status) || !CreateHashHandle(context))
    {
        DestroyHashContext(context);
        return nullptr;
    }

    return context;
}

bool ResetHashContext(HashContext* context) noexcept
{
    if (context == nullptr || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    if (context->hash != nullptr)
    {
        BCryptDestroyHash(context->hash);
        context->hash = nullptr;
    }
    return CreateHashHandle(context);
}

bool DuplicateHashContext(
    const HashContext* source,
    HashContext* destination) noexcept
{
    if (source == nullptr || destination == nullptr ||
        source->digestLength != destination->digestLength ||
        source->objectLength != destination->objectLength ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    if (destination->hash != nullptr)
    {
        BCryptDestroyHash(destination->hash);
        destination->hash = nullptr;
    }

    return NT_SUCCESS(BCryptDuplicateHash(
        source->hash,
        &destination->hash,
        destination->object,
        destination->objectLength,
        0));
}

bool UpdateHashContext(
    HashContext* context,
    const void* data,
    size_t dataLength) noexcept
{
    if (context == nullptr || (data == nullptr && dataLength != 0) ||
        dataLength > MAXULONG || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    return NT_SUCCESS(BCryptHashData(
        context->hash,
        static_cast<PUCHAR>(const_cast<void*>(data)),
        static_cast<ULONG>(dataLength),
        0));
}

bool FinishHashContext(HashContext* context, NvU8* digest) noexcept
{
    return context != nullptr && digest != nullptr &&
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
        NT_SUCCESS(BCryptFinishHash(
            context->hash,
            digest,
            context->digestLength,
            0));
}

bool SetHmacKey(
    HashContext* context,
    const NvU8* key,
    size_t keyLength) noexcept
{
    if (context == nullptr || (key == nullptr && keyLength != 0) ||
        keyLength > MAXULONG || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    if (context->hash != nullptr)
    {
        BCryptDestroyHash(context->hash);
        context->hash = nullptr;
    }

    return CreateHashHandle(context, key, static_cast<ULONG>(keyLength));
}

bool HashAll(
    PCWSTR algorithmName,
    ULONG digestLength,
    const void* data,
    size_t dataLength,
    NvU8* digest) noexcept
{
    if (digest == nullptr || (data == nullptr && dataLength != 0) ||
        dataLength > MAXULONG)
    {
        return false;
    }

    HashContext* context = CreateHashContext(algorithmName, digestLength);
    if (context == nullptr)
    {
        return false;
    }

    const bool result = UpdateHashContext(context, data, dataLength) &&
        FinishHashContext(context, digest);
    DestroyHashContext(context);
    return result;
}

bool HmacAll(
    PCWSTR algorithmName,
    ULONG digestLength,
    const void* data,
    size_t dataLength,
    const NvU8* key,
    size_t keyLength,
    NvU8* digest) noexcept
{
    if (digest == nullptr || (data == nullptr && dataLength != 0) ||
        (key == nullptr && keyLength != 0) || dataLength > MAXULONG ||
        keyLength > MAXULONG)
    {
        return false;
    }

    HashContext* context = CreateHashContext(
        algorithmName,
        digestLength,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (context == nullptr)
    {
        return false;
    }

    const bool result = SetHmacKey(context, key, keyLength) &&
        UpdateHashContext(context, data, dataLength) &&
        FinishHashContext(context, digest);
    DestroyHashContext(context);
    return result;
}

bool HkdfExpand(
    PCWSTR algorithmName,
    ULONG digestLength,
    const NvU8* prk,
    size_t prkLength,
    const NvU8* info,
    size_t infoLength,
    NvU8* output,
    size_t outputLength) noexcept
{
    if (prk == nullptr || info == nullptr || output == nullptr ||
        prkLength != digestLength || infoLength > MAXULONG ||
        outputLength > static_cast<size_t>(digestLength) * 255)
    {
        return false;
    }

    HashContext* context = CreateHashContext(
        algorithmName,
        digestLength,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (context == nullptr)
    {
        return false;
    }

    NvU8 previous[48]{};
    size_t produced = 0;
    NvU8 counter = 1;
    bool result = true;
    while (produced < outputLength)
    {
        result = SetHmacKey(context, prk, prkLength);
        if (result && produced != 0)
        {
            result = UpdateHashContext(context, previous, digestLength);
        }
        if (result && infoLength != 0)
        {
            result = UpdateHashContext(context, info, infoLength);
        }
        if (result)
        {
            result = UpdateHashContext(context, &counter, sizeof(counter)) &&
                FinishHashContext(context, previous);
        }
        if (!result)
        {
            break;
        }

        const size_t remaining = outputLength - produced;
        const size_t copyLength = remaining < digestLength
            ? remaining
            : digestLength;
        RtlCopyMemory(output + produced, previous, copyLength);
        produced += copyLength;
        ++counter;
    }

    RtlSecureZeroMemory(previous, sizeof(previous));
    DestroyHashContext(context);
    return result;
}

AesGcmContext* CreateAesGcmContext() noexcept
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return nullptr;
    }

    auto* context = static_cast<AesGcmContext*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(AesGcmContext),
        CryptoPoolTag));
    if (context == nullptr)
    {
        return nullptr;
    }
    context->algorithm = nullptr;

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &context->algorithm,
        BCRYPT_AES_ALGORITHM,
        nullptr,
        0);
    if (NT_SUCCESS(status))
    {
        status = BCryptSetProperty(
            context->algorithm,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<PWSTR>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM),
            0);
    }
    if (!NT_SUCCESS(status))
    {
        if (context->algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(context->algorithm, 0);
        }
        ExFreePoolWithTag(context, CryptoPoolTag);
        return nullptr;
    }

    return context;
}

void DestroyAesGcmContext(AesGcmContext* context) noexcept
{
    if (context != nullptr)
    {
        if (context->algorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(context->algorithm, 0);
        }
        ExFreePoolWithTag(context, CryptoPoolTag);
    }
}

bool AesGcmCrypt(
    AesGcmContext* context,
    const NvU8* key,
    size_t keyLength,
    const NvU8* iv,
    size_t ivLength,
    const NvU8* additionalData,
    size_t additionalDataLength,
    const NvU8* input,
    size_t inputLength,
    NvU8* tag,
    size_t tagLength,
    NvU8* output,
    size_t* outputLength,
    bool encrypt) noexcept
{
    if (context == nullptr || key == nullptr || iv == nullptr ||
        (additionalData == nullptr && additionalDataLength != 0) ||
        (input == nullptr && inputLength != 0) || tag == nullptr ||
        output == nullptr || outputLength == nullptr || ivLength != 12 ||
        (keyLength != 16 && keyLength != 24 && keyLength != 32) ||
        tagLength < 12 || tagLength > 16 || inputLength > MAXULONG ||
        additionalDataLength > MAXULONG || *outputLength < inputLength ||
        *outputLength > MAXULONG || KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    ULONG keyObjectLength = 0;
    ULONG resultLength = 0;
    NTSTATUS status = BCryptGetProperty(
        context->algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&keyObjectLength),
        sizeof(keyObjectLength),
        &resultLength,
        0);
    PUCHAR keyObject = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (NT_SUCCESS(status))
    {
        keyObject = static_cast<PUCHAR>(ExAllocatePool2(
            POOL_FLAG_NON_PAGED,
            keyObjectLength,
            CryptoPoolTag));
        if (keyObject == nullptr)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (NT_SUCCESS(status))
    {
        status = BCryptGenerateSymmetricKey(
            context->algorithm,
            &keyHandle,
            keyObject,
            keyObjectLength,
            const_cast<PUCHAR>(key),
            static_cast<ULONG>(keyLength),
            0);
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo{};
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(iv);
    authInfo.cbNonce = static_cast<ULONG>(ivLength);
    authInfo.pbAuthData = const_cast<PUCHAR>(additionalData);
    authInfo.cbAuthData = static_cast<ULONG>(additionalDataLength);
    authInfo.pbTag = tag;
    authInfo.cbTag = static_cast<ULONG>(tagLength);

    ULONG produced = 0;
    if (NT_SUCCESS(status))
    {
        status = encrypt
            ? BCryptEncrypt(
                keyHandle,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputLength),
                &authInfo,
                nullptr,
                0,
                output,
                static_cast<ULONG>(*outputLength),
                &produced,
                0)
            : BCryptDecrypt(
                keyHandle,
                const_cast<PUCHAR>(input),
                static_cast<ULONG>(inputLength),
                &authInfo,
                nullptr,
                0,
                output,
                static_cast<ULONG>(*outputLength),
                &produced,
                0);
    }

    if (keyHandle != nullptr)
    {
        BCryptDestroyKey(keyHandle);
    }
    if (keyObject != nullptr)
    {
        RtlSecureZeroMemory(keyObject, keyObjectLength);
        ExFreePoolWithTag(keyObject, CryptoPoolTag);
    }

    if (!NT_SUCCESS(status) || produced != inputLength)
    {
        return false;
    }

    *outputLength = produced;
    return true;
}

PCWSTR GetEcdhAlgorithm(const EcContext* context) noexcept
{
    return context->coordinateLength == 32
        ? BCRYPT_ECDH_P256_ALGORITHM
        : BCRYPT_ECDH_P384_ALGORITHM;
}

PCWSTR GetEcdsaAlgorithm(const EcContext* context) noexcept
{
    return context->coordinateLength == 32
        ? BCRYPT_ECDSA_P256_ALGORITHM
        : BCRYPT_ECDSA_P384_ALGORITHM;
}

ULONG GetEcdhPublicMagic(const EcContext* context) noexcept
{
    return context->coordinateLength == 32
        ? BCRYPT_ECDH_PUBLIC_P256_MAGIC
        : BCRYPT_ECDH_PUBLIC_P384_MAGIC;
}

ULONG GetEcdsaPrivateMagic(const EcContext* context) noexcept
{
    return context->coordinateLength == 32
        ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC
        : BCRYPT_ECDSA_PRIVATE_P384_MAGIC;
}

ULONG GetEcdsaPublicMagic(const EcContext* context) noexcept
{
    return context->coordinateLength == 32
        ? BCRYPT_ECDSA_PUBLIC_P256_MAGIC
        : BCRYPT_ECDSA_PUBLIC_P384_MAGIC;
}

bool OpenAlgorithm(PCWSTR name, BCRYPT_ALG_HANDLE* algorithm) noexcept
{
    return KeGetCurrentIrql() == PASSIVE_LEVEL &&
        NT_SUCCESS(BCryptOpenAlgorithmProvider(algorithm, name, nullptr, 0));
}

bool ImportEcKey(
    const EcContext* context,
    PCWSTR algorithmName,
    ULONG magic,
    const NvU8* keyData,
    ULONG keyDataLength,
    BCRYPT_ALG_HANDLE* algorithm,
    BCRYPT_KEY_HANDLE* key) noexcept
{
    if (!OpenAlgorithm(algorithmName, algorithm))
    {
        return false;
    }

    NvU8 blob[sizeof(BCRYPT_ECCKEY_BLOB) + 3 * 48]{};
    if (keyDataLength > sizeof(blob) - sizeof(BCRYPT_ECCKEY_BLOB))
    {
        BCryptCloseAlgorithmProvider(*algorithm, 0);
        *algorithm = nullptr;
        return false;
    }

    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = magic;
    header->cbKey = context->coordinateLength;
    RtlCopyMemory(blob + sizeof(*header), keyData, keyDataLength);
    LPCWSTR blobType = keyDataLength == 2 * context->coordinateLength
        ? BCRYPT_ECCPUBLIC_BLOB
        : BCRYPT_ECCPRIVATE_BLOB;
    const NTSTATUS status = BCryptImportKeyPair(
        *algorithm,
        nullptr,
        blobType,
        key,
        blob,
        sizeof(*header) + keyDataLength,
        0);
    RtlSecureZeroMemory(blob, sizeof(blob));
    if (!NT_SUCCESS(status))
    {
        BCryptCloseAlgorithmProvider(*algorithm, 0);
        *algorithm = nullptr;
        return false;
    }
    return true;
}

void CloseEcKey(
    BCRYPT_ALG_HANDLE algorithm,
    BCRYPT_KEY_HANDLE key) noexcept
{
    if (key != nullptr)
    {
        BCryptDestroyKey(key);
    }
    if (algorithm != nullptr)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
}

} // namespace

extern "C" NV_STATUS NvPlatformRmGetRandomBytes(
    NvU8* bytes,
    NvU32 byteCount)
{
    if ((bytes == nullptr && byteCount != 0) ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (byteCount == 0)
    {
        return NV_OK;
    }

    return NT_SUCCESS(BCryptGenRandom(
        nullptr,
        bytes,
        byteCount,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        ? NV_OK
        : NV_ERR_OPERATING_SYSTEM;
}

extern "C" void* NvPlatformRmCreateEcPublicKey(
    NvU32 curveNid,
    const NvU8* publicKey,
    NvU32 publicKeyLength)
{
    EcContext* context = CreateEcContext(curveNid);
    if (context == nullptr || publicKey == nullptr ||
        publicKeyLength != 2 * context->coordinateLength)
    {
        DestroyEcContext(context);
        return nullptr;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ImportEcKey(
            context,
            GetEcdsaAlgorithm(context),
            GetEcdsaPublicMagic(context),
            publicKey,
            publicKeyLength,
            &algorithm,
            &key))
    {
        DestroyEcContext(context);
        return nullptr;
    }
    CloseEcKey(algorithm, key);

    RtlCopyMemory(context->publicData, publicKey, publicKeyLength);
    context->publicDataLength = publicKeyLength;
    return context;
}

extern "C" void* NvPlatformRmCreateRsaPublicKey(
    const NvU8* modulus,
    NvU32 modulusLength,
    const NvU8* exponent,
    NvU32 exponentLength)
{
    if (modulus == nullptr || exponent == nullptr ||
        (modulusLength != 256 && modulusLength != 384 &&
         modulusLength != 512) ||
        exponentLength == 0 || exponentLength > sizeof(ULONGLONG) ||
        exponentLength > MAXULONG - sizeof(BCRYPT_RSAKEY_BLOB) -
            modulusLength ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return nullptr;
    }

    auto* context = static_cast<RsaContext*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(RsaContext),
        CryptoPoolTag));
    if (context == nullptr)
    {
        return nullptr;
    }
    RtlZeroMemory(context, sizeof(*context));
    context->modulusLength = modulusLength;

    const ULONG blobLength = sizeof(BCRYPT_RSAKEY_BLOB) +
        exponentLength + modulusLength;
    auto* blob = static_cast<PUCHAR>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        blobLength,
        CryptoPoolTag));
    if (blob == nullptr)
    {
        ExFreePoolWithTag(context, CryptoPoolTag);
        return nullptr;
    }

    auto* header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob);
    RtlZeroMemory(header, sizeof(*header));
    header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header->BitLength = modulusLength * 8;
    header->cbPublicExp = exponentLength;
    header->cbModulus = modulusLength;
    RtlCopyMemory(blob + sizeof(*header), exponent, exponentLength);
    RtlCopyMemory(
        blob + sizeof(*header) + exponentLength,
        modulus,
        modulusLength);

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &context->algorithm,
        BCRYPT_RSA_ALGORITHM,
        nullptr,
        0);
    if (NT_SUCCESS(status))
    {
        status = BCryptImportKeyPair(
            context->algorithm,
            nullptr,
            BCRYPT_RSAPUBLIC_BLOB,
            &context->key,
            blob,
            blobLength,
            0);
    }
    RtlSecureZeroMemory(blob, blobLength);
    ExFreePoolWithTag(blob, CryptoPoolTag);

    if (!NT_SUCCESS(status))
    {
        NvPlatformRmDestroyRsaKey(context);
        return nullptr;
    }
    return context;
}

extern "C" void NvPlatformRmDestroyRsaKey(void* rsaContext)
{
    auto* context = static_cast<RsaContext*>(rsaContext);
    if (context == nullptr)
    {
        return;
    }
    if (context->key != nullptr)
    {
        BCryptDestroyKey(context->key);
    }
    if (context->algorithm != nullptr)
    {
        BCryptCloseAlgorithmProvider(context->algorithm, 0);
    }
    RtlSecureZeroMemory(context, sizeof(*context));
    ExFreePoolWithTag(context, CryptoPoolTag);
}

extern "C" NvBool NvPlatformRmVerifyRsaPss(
    void* rsaContext,
    NvU32 hashNid,
    const NvU8* hash,
    NvU32 hashLength,
    const NvU8* signature,
    NvU32 signatureLength)
{
    auto* context = static_cast<RsaContext*>(rsaContext);
    PCWSTR algorithmName;
    NvU32 requiredHashLength;
    switch (hashNid)
    {
        case 0x0001:
            algorithmName = BCRYPT_SHA256_ALGORITHM;
            requiredHashLength = 32;
            break;
        case 0x0002:
            algorithmName = BCRYPT_SHA384_ALGORITHM;
            requiredHashLength = 48;
            break;
        case 0x0003:
            algorithmName = BCRYPT_SHA512_ALGORITHM;
            requiredHashLength = 64;
            break;
        default:
            return NV_FALSE;
    }

    if (context == nullptr || hash == nullptr || signature == nullptr ||
        hashLength != requiredHashLength ||
        signatureLength != context->modulusLength ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_FALSE;
    }

    BCRYPT_PSS_PADDING_INFO paddingInfo{};
    paddingInfo.pszAlgId = algorithmName;
    paddingInfo.cbSalt = requiredHashLength;
    return NT_SUCCESS(BCryptVerifySignature(
        context->key,
        &paddingInfo,
        const_cast<PUCHAR>(hash),
        hashLength,
        const_cast<PUCHAR>(signature),
        signatureLength,
        BCRYPT_PAD_PSS))
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" NvBool NvPlatformRmVerifyRsaPkcs1(
    void* rsaContext,
    NvU32 hashNid,
    const NvU8* hash,
    NvU32 hashLength,
    const NvU8* signature,
    NvU32 signatureLength)
{
    auto* context = static_cast<RsaContext*>(rsaContext);
    PCWSTR algorithmName;
    NvU32 requiredHashLength;
    switch (hashNid)
    {
        case 0x0001:
            algorithmName = BCRYPT_SHA256_ALGORITHM;
            requiredHashLength = 32;
            break;
        case 0x0002:
            algorithmName = BCRYPT_SHA384_ALGORITHM;
            requiredHashLength = 48;
            break;
        default:
            return NV_FALSE;
    }

    if (context == nullptr || hash == nullptr || signature == nullptr ||
        hashLength != requiredHashLength ||
        signatureLength != context->modulusLength ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return NV_FALSE;
    }

    BCRYPT_PKCS1_PADDING_INFO paddingInfo{};
    paddingInfo.pszAlgId = algorithmName;
    return NT_SUCCESS(BCryptVerifySignature(
        context->key,
        &paddingInfo,
        const_cast<PUCHAR>(hash),
        hashLength,
        const_cast<PUCHAR>(signature),
        signatureLength,
        BCRYPT_PAD_PKCS1))
        ? NV_TRUE
        : NV_FALSE;
}

extern "C" void* libspdm_sha256_new(void)
{
    return CreateHashContext(BCRYPT_SHA256_ALGORITHM, 32);
}

extern "C" void libspdm_sha256_free(void* context)
{
    DestroyHashContext(static_cast<HashContext*>(context));
}

extern "C" bool libspdm_sha256_init(void* context)
{
    return ResetHashContext(static_cast<HashContext*>(context));
}

extern "C" bool libspdm_sha256_duplicate(
    const void* source,
    void* destination)
{
    return DuplicateHashContext(
        static_cast<const HashContext*>(source),
        static_cast<HashContext*>(destination));
}

extern "C" bool libspdm_sha256_update(
    void* context,
    const void* data,
    size_t dataLength)
{
    return UpdateHashContext(
        static_cast<HashContext*>(context),
        data,
        dataLength);
}

extern "C" bool libspdm_sha256_final(void* context, NvU8* digest)
{
    return FinishHashContext(static_cast<HashContext*>(context), digest);
}

extern "C" bool libspdm_sha256_hash_all(
    const void* data,
    size_t dataLength,
    NvU8* digest)
{
    return HashAll(BCRYPT_SHA256_ALGORITHM, 32, data, dataLength, digest);
}

extern "C" void* libspdm_sha384_new(void)
{
    return CreateHashContext(BCRYPT_SHA384_ALGORITHM, 48);
}

extern "C" void libspdm_sha384_free(void* context)
{
    DestroyHashContext(static_cast<HashContext*>(context));
}

extern "C" bool libspdm_sha384_init(void* context)
{
    return ResetHashContext(static_cast<HashContext*>(context));
}

extern "C" bool libspdm_sha384_duplicate(
    const void* source,
    void* destination)
{
    return DuplicateHashContext(
        static_cast<const HashContext*>(source),
        static_cast<HashContext*>(destination));
}

extern "C" bool libspdm_sha384_update(
    void* context,
    const void* data,
    size_t dataLength)
{
    return UpdateHashContext(
        static_cast<HashContext*>(context),
        data,
        dataLength);
}

extern "C" bool libspdm_sha384_final(void* context, NvU8* digest)
{
    return FinishHashContext(static_cast<HashContext*>(context), digest);
}

extern "C" bool libspdm_sha384_hash_all(
    const void* data,
    size_t dataLength,
    NvU8* digest)
{
    return HashAll(BCRYPT_SHA384_ALGORITHM, 48, data, dataLength, digest);
}

extern "C" void* libspdm_hmac_sha256_new(void)
{
    return CreateHashContext(
        BCRYPT_SHA256_ALGORITHM,
        32,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
}

extern "C" void libspdm_hmac_sha256_free(void* context)
{
    DestroyHashContext(static_cast<HashContext*>(context));
}

extern "C" bool libspdm_hmac_sha256_set_key(
    void* context,
    const NvU8* key,
    size_t keyLength)
{
    return SetHmacKey(static_cast<HashContext*>(context), key, keyLength);
}

extern "C" bool libspdm_hmac_sha256_duplicate(
    const void* source,
    void* destination)
{
    return DuplicateHashContext(
        static_cast<const HashContext*>(source),
        static_cast<HashContext*>(destination));
}

extern "C" bool libspdm_hmac_sha256_update(
    void* context,
    const void* data,
    size_t dataLength)
{
    return UpdateHashContext(
        static_cast<HashContext*>(context),
        data,
        dataLength);
}

extern "C" bool libspdm_hmac_sha256_final(void* context, NvU8* digest)
{
    return FinishHashContext(static_cast<HashContext*>(context), digest);
}

extern "C" bool libspdm_hmac_sha256_all(
    const void* data,
    size_t dataLength,
    const NvU8* key,
    size_t keyLength,
    NvU8* digest)
{
    return HmacAll(
        BCRYPT_SHA256_ALGORITHM,
        32,
        data,
        dataLength,
        key,
        keyLength,
        digest);
}

extern "C" void* libspdm_hmac_sha384_new(void)
{
    return CreateHashContext(
        BCRYPT_SHA384_ALGORITHM,
        48,
        BCRYPT_ALG_HANDLE_HMAC_FLAG);
}

extern "C" void libspdm_hmac_sha384_free(void* context)
{
    DestroyHashContext(static_cast<HashContext*>(context));
}

extern "C" bool libspdm_hmac_sha384_set_key(
    void* context,
    const NvU8* key,
    size_t keyLength)
{
    return SetHmacKey(static_cast<HashContext*>(context), key, keyLength);
}

extern "C" bool libspdm_hmac_sha384_duplicate(
    const void* source,
    void* destination)
{
    return DuplicateHashContext(
        static_cast<const HashContext*>(source),
        static_cast<HashContext*>(destination));
}

extern "C" bool libspdm_hmac_sha384_update(
    void* context,
    const void* data,
    size_t dataLength)
{
    return UpdateHashContext(
        static_cast<HashContext*>(context),
        data,
        dataLength);
}

extern "C" bool libspdm_hmac_sha384_final(void* context, NvU8* digest)
{
    return FinishHashContext(static_cast<HashContext*>(context), digest);
}

extern "C" bool libspdm_hmac_sha384_all(
    const void* data,
    size_t dataLength,
    const NvU8* key,
    size_t keyLength,
    NvU8* digest)
{
    return HmacAll(
        BCRYPT_SHA384_ALGORITHM,
        48,
        data,
        dataLength,
        key,
        keyLength,
        digest);
}

extern "C" bool libspdm_hkdf_sha256_extract(
    const NvU8* key,
    size_t keyLength,
    const NvU8* salt,
    size_t saltLength,
    NvU8* prk,
    size_t prkLength)
{
    return prkLength == 32 && HmacAll(
        BCRYPT_SHA256_ALGORITHM,
        32,
        key,
        keyLength,
        salt,
        saltLength,
        prk);
}

extern "C" bool libspdm_hkdf_sha256_expand(
    const NvU8* prk,
    size_t prkLength,
    const NvU8* info,
    size_t infoLength,
    NvU8* output,
    size_t outputLength)
{
    return HkdfExpand(
        BCRYPT_SHA256_ALGORITHM,
        32,
        prk,
        prkLength,
        info,
        infoLength,
        output,
        outputLength);
}

extern "C" bool libspdm_hkdf_sha384_extract(
    const NvU8* key,
    size_t keyLength,
    const NvU8* salt,
    size_t saltLength,
    NvU8* prk,
    size_t prkLength)
{
    return prkLength == 48 && HmacAll(
        BCRYPT_SHA384_ALGORITHM,
        48,
        key,
        keyLength,
        salt,
        saltLength,
        prk);
}

extern "C" bool libspdm_hkdf_sha384_expand(
    const NvU8* prk,
    size_t prkLength,
    const NvU8* info,
    size_t infoLength,
    NvU8* output,
    size_t outputLength)
{
    return HkdfExpand(
        BCRYPT_SHA384_ALGORITHM,
        48,
        prk,
        prkLength,
        info,
        infoLength,
        output,
        outputLength);
}

extern "C" bool libspdm_aead_gcm_prealloc(void** context)
{
    if (context == nullptr)
    {
        return false;
    }

    *context = CreateAesGcmContext();
    return *context != nullptr;
}

extern "C" void libspdm_aead_free(void* context)
{
    DestroyAesGcmContext(static_cast<AesGcmContext*>(context));
}

extern "C" bool libspdm_aead_aes_gcm_encrypt_prealloc(
    void* context,
    const NvU8* key,
    size_t keyLength,
    const NvU8* iv,
    size_t ivLength,
    const NvU8* additionalData,
    size_t additionalDataLength,
    const NvU8* input,
    size_t inputLength,
    NvU8* tag,
    size_t tagLength,
    NvU8* output,
    size_t* outputLength)
{
    return AesGcmCrypt(
        static_cast<AesGcmContext*>(context),
        key,
        keyLength,
        iv,
        ivLength,
        additionalData,
        additionalDataLength,
        input,
        inputLength,
        tag,
        tagLength,
        output,
        outputLength,
        true);
}

extern "C" bool libspdm_aead_aes_gcm_decrypt_prealloc(
    void* context,
    const NvU8* key,
    size_t keyLength,
    const NvU8* iv,
    size_t ivLength,
    const NvU8* additionalData,
    size_t additionalDataLength,
    const NvU8* input,
    size_t inputLength,
    const NvU8* tag,
    size_t tagLength,
    NvU8* output,
    size_t* outputLength)
{
    return AesGcmCrypt(
        static_cast<AesGcmContext*>(context),
        key,
        keyLength,
        iv,
        ivLength,
        additionalData,
        additionalDataLength,
        input,
        inputLength,
        const_cast<NvU8*>(tag),
        tagLength,
        output,
        outputLength,
        false);
}

extern "C" bool libspdm_aead_aes_gcm_encrypt(
    const NvU8* key,
    size_t keyLength,
    const NvU8* iv,
    size_t ivLength,
    const NvU8* additionalData,
    size_t additionalDataLength,
    const NvU8* input,
    size_t inputLength,
    NvU8* tag,
    size_t tagLength,
    NvU8* output,
    size_t* outputLength)
{
    AesGcmContext* context = CreateAesGcmContext();
    const bool result = AesGcmCrypt(
        context,
        key,
        keyLength,
        iv,
        ivLength,
        additionalData,
        additionalDataLength,
        input,
        inputLength,
        tag,
        tagLength,
        output,
        outputLength,
        true);
    DestroyAesGcmContext(context);
    return result;
}

extern "C" bool libspdm_aead_aes_gcm_decrypt(
    const NvU8* key,
    size_t keyLength,
    const NvU8* iv,
    size_t ivLength,
    const NvU8* additionalData,
    size_t additionalDataLength,
    const NvU8* input,
    size_t inputLength,
    const NvU8* tag,
    size_t tagLength,
    NvU8* output,
    size_t* outputLength)
{
    AesGcmContext* context = CreateAesGcmContext();
    const bool result = AesGcmCrypt(
        context,
        key,
        keyLength,
        iv,
        ivLength,
        additionalData,
        additionalDataLength,
        input,
        inputLength,
        const_cast<NvU8*>(tag),
        tagLength,
        output,
        outputLength,
        false);
    DestroyAesGcmContext(context);
    return result;
}

extern "C" void* libspdm_ec_new_by_nid(size_t nid)
{
    return CreateEcContext(nid);
}

extern "C" void libspdm_ec_free(void* ecContext)
{
    DestroyEcContext(static_cast<EcContext*>(ecContext));
}

extern "C" bool libspdm_ec_generate_key(
    void* ecContext,
    NvU8* publicData,
    size_t* publicLength)
{
    auto* context = static_cast<EcContext*>(ecContext);
    if (context == nullptr || publicLength == nullptr ||
        (publicData == nullptr && *publicLength != 0) ||
        *publicLength < 2 * context->coordinateLength ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm,
        GetEcdhAlgorithm(context),
        nullptr,
        0);
    if (NT_SUCCESS(status))
    {
        status = BCryptGenerateKeyPair(
            algorithm,
            &key,
            context->coordinateLength * 8,
            0);
    }
    if (NT_SUCCESS(status))
    {
        status = BCryptFinalizeKeyPair(key, 0);
    }

    ULONG blobLength = 0;
    if (NT_SUCCESS(status))
    {
        status = BCryptExportKey(
            key,
            nullptr,
            BCRYPT_ECCPRIVATE_BLOB,
            context->privateBlob,
            sizeof(context->privateBlob),
            &blobLength,
            0);
    }
    if (NT_SUCCESS(status) &&
        blobLength == sizeof(BCRYPT_ECCKEY_BLOB) +
            3 * context->coordinateLength)
    {
        context->privateBlobLength = blobLength;
        RtlCopyMemory(
            context->publicData,
            context->privateBlob + sizeof(BCRYPT_ECCKEY_BLOB),
            2 * context->coordinateLength);
        context->publicDataLength = 2 * context->coordinateLength;
        RtlCopyMemory(
            publicData,
            context->publicData,
            context->publicDataLength);
        *publicLength = 2 * context->coordinateLength;
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;
    }

    CloseEcKey(algorithm, key);
    return NT_SUCCESS(status);
}

extern "C" bool libspdm_ec_compute_key(
    void* ecContext,
    const NvU8* peerPublic,
    size_t peerPublicLength,
    NvU8* secret,
    size_t* secretLength)
{
    auto* context = static_cast<EcContext*>(ecContext);
    if (context == nullptr || peerPublic == nullptr || secret == nullptr ||
        secretLength == nullptr || context->privateBlobLength == 0 ||
        peerPublicLength != 2 * context->coordinateLength ||
        *secretLength < context->coordinateLength ||
        KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE privateKey = nullptr;
    BCRYPT_KEY_HANDLE publicKey = nullptr;
    BCRYPT_SECRET_HANDLE agreement = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm,
        GetEcdhAlgorithm(context),
        nullptr,
        0);
    if (NT_SUCCESS(status))
    {
        status = BCryptImportKeyPair(
            algorithm,
            nullptr,
            BCRYPT_ECCPRIVATE_BLOB,
            &privateKey,
            context->privateBlob,
            context->privateBlobLength,
            0);
    }

    NvU8 peerBlob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * 48]{};
    auto* peerHeader = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(peerBlob);
    peerHeader->dwMagic = GetEcdhPublicMagic(context);
    peerHeader->cbKey = context->coordinateLength;
    RtlCopyMemory(peerBlob + sizeof(*peerHeader), peerPublic, peerPublicLength);
    if (NT_SUCCESS(status))
    {
        status = BCryptImportKeyPair(
            algorithm,
            nullptr,
            BCRYPT_ECCPUBLIC_BLOB,
            &publicKey,
            peerBlob,
            sizeof(*peerHeader) + static_cast<ULONG>(peerPublicLength),
            0);
    }
    if (NT_SUCCESS(status))
    {
        status = BCryptSecretAgreement(privateKey, publicKey, &agreement, 0);
    }

    ULONG produced = 0;
    if (NT_SUCCESS(status))
    {
        status = BCryptDeriveKey(
            agreement,
            BCRYPT_KDF_RAW_SECRET,
            nullptr,
            secret,
            context->coordinateLength,
            &produced,
            0);
    }
    if (NT_SUCCESS(status) && produced == context->coordinateLength)
    {
        for (ULONG left = 0, right = produced - 1; left < right;
             ++left, --right)
        {
            const NvU8 value = secret[left];
            secret[left] = secret[right];
            secret[right] = value;
        }
        *secretLength = produced;
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;
    }

    if (agreement != nullptr)
    {
        BCryptDestroySecret(agreement);
    }
    CloseEcKey(nullptr, publicKey);
    CloseEcKey(algorithm, privateKey);
    return NT_SUCCESS(status);
}

extern "C" bool libspdm_ecdsa_sign(
    void* ecContext,
    size_t hashNid,
    const NvU8* hash,
    size_t hashLength,
    NvU8* signature,
    size_t* signatureLength)
{
    auto* context = static_cast<EcContext*>(ecContext);
    if (context == nullptr || hash == nullptr || signature == nullptr ||
        signatureLength == nullptr || context->privateBlobLength == 0 ||
        ((hashNid == 0x0001 && hashLength != 32) ||
         (hashNid == 0x0002 && hashLength != 48) ||
         (hashNid != 0x0001 && hashNid != 0x0002)) ||
        *signatureLength < 2 * context->coordinateLength ||
        hashLength > MAXULONG)
    {
        return false;
    }

    const NvU8* keyData = context->privateBlob + sizeof(BCRYPT_ECCKEY_BLOB);
    const ULONG keyDataLength = context->privateBlobLength -
        sizeof(BCRYPT_ECCKEY_BLOB);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ImportEcKey(
        context,
        GetEcdsaAlgorithm(context),
        GetEcdsaPrivateMagic(context),
        keyData,
        keyDataLength,
        &algorithm,
        &key))
    {
        return false;
    }

    ULONG produced = 0;
    const NTSTATUS status = BCryptSignHash(
        key,
        nullptr,
        const_cast<PUCHAR>(hash),
        static_cast<ULONG>(hashLength),
        signature,
        static_cast<ULONG>(*signatureLength),
        &produced,
        0);
    CloseEcKey(algorithm, key);
    if (!NT_SUCCESS(status))
    {
        return false;
    }

    *signatureLength = produced;
    return produced == 2 * context->coordinateLength;
}

extern "C" bool libspdm_ecdsa_verify(
    void* ecContext,
    size_t hashNid,
    const NvU8* hash,
    size_t hashLength,
    const NvU8* signature,
    size_t signatureLength)
{
    auto* context = static_cast<EcContext*>(ecContext);
    if (context == nullptr || hash == nullptr || signature == nullptr ||
        context->publicDataLength != 2 * context->coordinateLength ||
        signatureLength != 2 * context->coordinateLength ||
        ((hashNid == 0x0001 && hashLength != 32) ||
         (hashNid == 0x0002 && hashLength != 48) ||
         (hashNid != 0x0001 && hashNid != 0x0002)) ||
        hashLength > MAXULONG)
    {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ImportEcKey(
        context,
        GetEcdsaAlgorithm(context),
        GetEcdsaPublicMagic(context),
        context->publicData,
        context->publicDataLength,
        &algorithm,
        &key))
    {
        return false;
    }

    const NTSTATUS status = BCryptVerifySignature(
        key,
        nullptr,
        const_cast<PUCHAR>(hash),
        static_cast<ULONG>(hashLength),
        const_cast<PUCHAR>(signature),
        static_cast<ULONG>(signatureLength),
        0);
    CloseEcKey(algorithm, key);
    return NT_SUCCESS(status);
}