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
    void* rmGpu;
    volatile LONG state;
};

extern "C" NV_STATUS NvPlatformRmGpuContextCreate(
    void* adapterContext,
    NV_PLATFORM_RM_GPU_CONTEXT** gpuContext)
{
    if (adapterContext == nullptr || gpuContext == nullptr)
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
    (*gpuContext)->rmGpu = nullptr;
    (*gpuContext)->state = static_cast<LONG>(GpuContextState::Available);
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