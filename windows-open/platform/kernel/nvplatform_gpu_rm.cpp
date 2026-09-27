#include <ntddk.h>

namespace {

constexpr ULONG GpuContextPoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('G') << 16) |
    (static_cast<ULONG>('c') << 24);

void* NvPlatformGpuContextAllocate(SIZE_T size) noexcept
{
    return ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        size,
        GpuContextPoolTag);
}

void NvPlatformGpuContextFree(void* allocation) noexcept
{
    ExFreePoolWithTag(allocation, GpuContextPoolTag);
}

} // namespace

#define NV_PLATFORM_ARRAY_COUNT RTL_NUMBER_OF
#define NV_PLATFORM_ASSERT NT_ASSERT
#include "..\shared\nvplatform_gpu_rm_impl.inl"
