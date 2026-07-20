#include <ntddk.h>

#include "nvplatform_gpu_rm.h"

namespace {

constexpr ULONG GpuContextPoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('G') << 16) |
    (static_cast<ULONG>('c') << 24);

enum class GpuContextState : LONG
{
    Available,
    Reserved,
    Published,
};

} // namespace

struct NV_PLATFORM_RM_GPU_CONTEXT
{
    void* adapterContext;
    NV_PLATFORM_RM_GPU_ATTACH_INFO attachInfo;
    void* rmGpu;
    volatile LONG state;
    volatile LONG lost;
};

extern "C" NV_STATUS NvPlatformRmGpuContextCreate(
    void* adapterContext,
    const NV_PLATFORM_RM_GPU_ATTACH_INFO* attachInfo,
    NV_PLATFORM_RM_GPU_CONTEXT** gpuContext)
{
    if (adapterContext == nullptr || attachInfo == nullptr || gpuContext == nullptr ||
        attachInfo->apertureCount == 0 ||
        attachInfo->apertureCount > RTL_NUMBER_OF(attachInfo->apertures) ||
        attachInfo->apertures[0].physicalAddress == 0 ||
        attachInfo->apertures[0].kernelAddress == nullptr ||
        attachInfo->apertures[0].length == 0)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *gpuContext = static_cast<NV_PLATFORM_RM_GPU_CONTEXT*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(NV_PLATFORM_RM_GPU_CONTEXT),
        GpuContextPoolTag));
    if (*gpuContext == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    (*gpuContext)->adapterContext = adapterContext;
    (*gpuContext)->attachInfo = *attachInfo;
    (*gpuContext)->rmGpu = nullptr;
    (*gpuContext)->state = static_cast<LONG>(GpuContextState::Available);
    (*gpuContext)->lost = FALSE;
    return NV_OK;
}

extern "C" void NvPlatformRmGpuContextDestroy(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext)
{
    if (gpuContext == nullptr)
    {
        return;
    }

    NT_ASSERT(gpuContext->state == static_cast<LONG>(GpuContextState::Available));
    ExFreePoolWithTag(gpuContext, GpuContextPoolTag);
}

extern "C" NV_STATUS NvPlatformRmGpuReserve(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu)
{
    if (gpuContext == nullptr || rmGpu == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const LONG previous = InterlockedCompareExchange(
        &gpuContext->state,
        static_cast<LONG>(GpuContextState::Reserved),
        static_cast<LONG>(GpuContextState::Available));
    if (previous != static_cast<LONG>(GpuContextState::Available))
    {
        return NV_ERR_STATE_IN_USE;
    }

    gpuContext->rmGpu = rmGpu;
    return NV_OK;
}

extern "C" NV_STATUS NvPlatformRmGpuPublish(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu)
{
    if (gpuContext == nullptr || gpuContext->rmGpu != rmGpu)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    const LONG previous = InterlockedCompareExchange(
        &gpuContext->state,
        static_cast<LONG>(GpuContextState::Published),
        static_cast<LONG>(GpuContextState::Reserved));
    return previous == static_cast<LONG>(GpuContextState::Reserved)
        ? NV_OK
        : NV_ERR_INVALID_STATE;
}

extern "C" NV_STATUS NvPlatformRmGpuGetPublishedRmGpu(
    const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void** rmGpu)
{
    if (gpuContext == nullptr || rmGpu == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    if (InterlockedCompareExchange(
            const_cast<volatile LONG*>(&gpuContext->state),
            static_cast<LONG>(GpuContextState::Published),
            static_cast<LONG>(GpuContextState::Published)) !=
        static_cast<LONG>(GpuContextState::Published))
    {
        return NV_ERR_INVALID_STATE;
    }

    *rmGpu = gpuContext->rmGpu;
    return *rmGpu != nullptr ? NV_OK : NV_ERR_INVALID_STATE;
}

extern "C" void NvPlatformRmGpuWithdraw(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    void* rmGpu)
{
    if (gpuContext == nullptr || gpuContext->rmGpu != rmGpu)
    {
        return;
    }

    gpuContext->rmGpu = nullptr;
    InterlockedExchange(
        &gpuContext->state,
        static_cast<LONG>(GpuContextState::Available));
}

extern "C" NV_STATUS NvPlatformRmGpuGetAttachInfo(
    const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext,
    NV_PLATFORM_RM_GPU_ATTACH_INFO* attachInfo)
{
    if (gpuContext == nullptr || attachInfo == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *attachInfo = gpuContext->attachInfo;
    return NV_OK;
}

extern "C" void NvPlatformRmGpuMarkLost(
    NV_PLATFORM_RM_GPU_CONTEXT* gpuContext)
{
    if (gpuContext != nullptr)
    {
        InterlockedExchange(&gpuContext->lost, TRUE);
    }
}

extern "C" NvBool NvPlatformRmGpuIsLost(
    const NV_PLATFORM_RM_GPU_CONTEXT* gpuContext)
{
    return gpuContext != nullptr &&
        InterlockedCompareExchange(
            const_cast<volatile LONG*>(&gpuContext->lost),
            TRUE,
            TRUE) != FALSE
        ? NV_TRUE
        : NV_FALSE;
}