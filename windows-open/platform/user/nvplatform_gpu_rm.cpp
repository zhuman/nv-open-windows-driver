#include <windows.h>
#include <intrin.h>

namespace {

void* NvPlatformGpuContextAllocate(SIZE_T size) noexcept
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void NvPlatformGpuContextFree(void* allocation) noexcept
{
    HeapFree(GetProcessHeap(), 0, allocation);
}

} // namespace

#define NV_PLATFORM_ARRAY_COUNT ARRAYSIZE
#define NV_PLATFORM_ASSERT(expression) \
    do { if (!(expression)) { __debugbreak(); } } while (0)
#include "..\shared\nvplatform_gpu_rm_impl.inl"