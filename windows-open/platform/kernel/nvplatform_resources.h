#pragma once

#include <ntddk.h>

#ifdef __cplusplus
extern "C" {
#define NV_PLATFORM_NOEXCEPT noexcept
#else
#define NV_PLATFORM_NOEXCEPT
#endif

enum
{
    NV_PLATFORM_MAX_MEMORY_RESOURCE_COUNT = 16,
    NV_PLATFORM_MAX_INTERRUPT_RESOURCE_COUNT = 32,
};

typedef struct NV_PLATFORM_MEMORY_RESOURCE
{
    PHYSICAL_ADDRESS physicalAddress;
    SIZE_T length;
    USHORT flags;
} NV_PLATFORM_MEMORY_RESOURCE;

typedef struct NV_PLATFORM_INTERRUPT_RESOURCE
{
    ULONG vector;
    ULONG level;
    KAFFINITY affinity;
    USHORT flags;
    BOOLEAN messageSignaled;
} NV_PLATFORM_INTERRUPT_RESOURCE;

typedef struct NV_PLATFORM_TRANSLATED_RESOURCES
{
    NV_PLATFORM_MEMORY_RESOURCE memory[NV_PLATFORM_MAX_MEMORY_RESOURCE_COUNT];
    ULONG memoryCount;
    NV_PLATFORM_INTERRUPT_RESOURCE interrupts[NV_PLATFORM_MAX_INTERRUPT_RESOURCE_COUNT];
    ULONG interruptCount;
} NV_PLATFORM_TRANSLATED_RESOURCES;

NTSTATUS NvPlatformDecodeTranslatedResources(
    _In_opt_ const CM_RESOURCE_LIST* translatedResources,
    _Out_ NV_PLATFORM_TRANSLATED_RESOURCES* resources) NV_PLATFORM_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef NV_PLATFORM_NOEXCEPT