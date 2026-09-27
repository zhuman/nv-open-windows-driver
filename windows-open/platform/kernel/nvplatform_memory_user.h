#pragma once

#include "nvplatform_memory_rm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef NV_STATUS (*NV_PLATFORM_DMA_ALLOCATE)(
    void* context,
    NvU64 size,
    NvU64 alignment,
    void** providerAllocation,
    void** address,
    NvU64* logicalAddress);

typedef void (*NV_PLATFORM_DMA_FREE)(
    void* context,
    void* providerAllocation);

typedef struct NV_PLATFORM_DMA_PROVIDER
{
    void* context;
    NV_PLATFORM_DMA_ALLOCATE allocate;
    NV_PLATFORM_DMA_FREE free;
} NV_PLATFORM_DMA_PROVIDER;

NV_STATUS NvPlatformRegisterDmaProvider(
    const NV_PLATFORM_DMA_PROVIDER* provider,
    void** registration) noexcept;

void NvPlatformUnregisterDmaProvider(void* registration) noexcept;

#ifdef __cplusplus
}
#endif