#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NV_COMMON_HOST_SERVICES_VERSION 4U

typedef void* (*NV_COMMON_ALLOCATE)(
    void* providerContext,
    NvU64 length,
    NvBool nonPaged);

typedef void (*NV_COMMON_FREE)(
    void* providerContext,
    void* allocation,
    NvBool nonPaged);

typedef NvU64 (*NV_COMMON_GET_MONOTONIC_TIME_100NS)(void* providerContext);

typedef NvU64 (*NV_COMMON_GET_UNIX_TIME_100NS)(void* providerContext);

typedef NvBool (*NV_COMMON_IS_ALLOCATION_SAFE)(
    void* providerContext,
    NvBool nonPaged);

typedef void (*NV_COMMON_LOG)(
    void* providerContext,
    NvU32 level,
    const char* message);

typedef struct NV_COMMON_HOST_SERVICES
{
    NvU32 version;
    NvU32 size;
    void* providerContext;
    NV_COMMON_ALLOCATE allocate;
    NV_COMMON_FREE free;
    NV_COMMON_GET_MONOTONIC_TIME_100NS getMonotonicTime100ns;
    NV_COMMON_GET_UNIX_TIME_100NS getUnixTime100ns;
    NV_COMMON_IS_ALLOCATION_SAFE isAllocationSafe;
    NV_COMMON_LOG log;
} NV_COMMON_HOST_SERVICES;

typedef struct NV_COMMON_HOST_CONTEXT
{
    NV_COMMON_HOST_SERVICES services;
    NvBool initialized;
} NV_COMMON_HOST_CONTEXT;

NV_STATUS nvCommonHostInitialize(
    NV_COMMON_HOST_CONTEXT* context,
    const NV_COMMON_HOST_SERVICES* services);

void nvCommonHostShutdown(NV_COMMON_HOST_CONTEXT* context);

void* nvCommonHostAllocate(
    const NV_COMMON_HOST_CONTEXT* context,
    NvU64 length,
    NvBool nonPaged);

void nvCommonHostFree(
    const NV_COMMON_HOST_CONTEXT* context,
    void* allocation,
    NvBool nonPaged);

NvU64 nvCommonHostGetMonotonicTime100ns(
    const NV_COMMON_HOST_CONTEXT* context);

NvU64 nvCommonHostGetUnixTime100ns(
    const NV_COMMON_HOST_CONTEXT* context);

NvBool nvCommonHostIsAllocationSafe(
    const NV_COMMON_HOST_CONTEXT* context,
    NvBool nonPaged);

void nvCommonHostLog(
    const NV_COMMON_HOST_CONTEXT* context,
    NvU32 level,
    const char* message);

#ifdef __cplusplus
}
#endif