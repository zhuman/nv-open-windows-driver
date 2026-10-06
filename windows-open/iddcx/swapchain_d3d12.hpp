#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d12.h>
#include <wil/com.h>
#include <wil/resource.h>

#include "nvkms_windows.h"
#include "swapchain_damage.hpp"

namespace nv::iddcx {

class Output;
class SwapChainD3D12;

struct SwapChainD3D12FailureContext final
{
    SwapChainD3D12* owner;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
    SwapChainD3D12FailureContext, GetSwapChainD3D12FailureContext);

class SwapChainD3D12 final
{
public:
    static NTSTATUS Create(
        Output& output,
        const IDARG_IN_SETSWAPCHAIN& arguments,
        NvU32 mode,
        SwapChainD3D12** swapChain) noexcept;
    ~SwapChainD3D12() noexcept;
    HRESULT StartProcessing() noexcept;
    void StopAndDelete() noexcept;
    bool Failed() const noexcept { return failed_; }

private:
    SwapChainD3D12(
        Output& output, IDDCX_SWAPCHAIN handle, HANDLE nextSurfaceEvent,
        NvU32 mode) noexcept;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    static EVT_WDF_WORKITEM FailureCleanup;
    static DWORD WINAPI ThreadEntry(void* context) noexcept;
    HRESULT Initialize(LUID adapterLuid) noexcept;
    HRESULT ImportBuffer(NvU32 index) noexcept;
    HRESULT CopyFrame(ID3D12Resource* source, NvU64 frameNumber) noexcept;
    HRESULT WaitForGpu() noexcept;
    void UnmapFrame() noexcept;
    void Process() noexcept;
    void Stop() noexcept;
    void UpdateHdrMetadata(const IDDCX_METADATA2& metadata) noexcept;
    NvU64 Microseconds(NvU64 ticks) const noexcept;
    void RecordTiming(
        NvU64 frameNumber, const NV_KMS_WINDOWS_PRESENT_TIMING& present,
        bool dropped, NvU64 totalTicks) noexcept;
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    Output* output_;
    IDDCX_SWAPCHAIN handle_;
    HANDLE nextSurfaceEvent_;
    NvU32 mode_;
    SwapChainDamage damage_;
    bool deletionStarted_ = false;
    bool failed_ = false;
    bool directRejected_ = false;
    WDFWORKITEM failureWorkItem_ = nullptr;
    wil::srwlock stateLock_;
    wil::unique_handle stopEvent_;
    wil::unique_handle thread_;
    wil::unique_handle fenceEvent_;
    wil::com_ptr_nothrow<ID3D12Device3> device_;
    wil::com_ptr_nothrow<ID3D12CommandQueue> queue_;
    wil::com_ptr_nothrow<ID3D12CommandAllocator> allocator_;
    wil::com_ptr_nothrow<ID3D12GraphicsCommandList> commands_;
    wil::com_ptr_nothrow<ID3D12Fence> fence_;
    NvU64 fenceValue_ = 0;
    wil::com_ptr_nothrow<ID3D12Resource> readback_;
    D3D12_RESOURCE_DESC readbackSourceDesc_ = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint_ = {};
    NvU64 readbackSize_ = 0;
    struct Buffer
    {
        wil::com_ptr_nothrow<ID3D12Heap> heap;
        wil::com_ptr_nothrow<ID3D12Resource> resource;
        void* pixels = nullptr;
        NvU64 offset = 0;
        NvU64 size = 0;
        bool verified = false;
        bool partialVerified = false;
        NvU32 framesSinceVerification = 0;
    } buffers_[2];
    ID3D12Resource* mappedResource_ = nullptr;
    LARGE_INTEGER counterFrequency_ = {};
    NV_KMS_WINDOWS_HDR_METADATA currentHdrMetadata_ = {};
    bool hasCurrentHdrMetadata_ = false;
    struct
    {
        NvU64 stagingTicks;
        NvU64 readbackTicks;
        NvU64 mapTicks;
        NvU64 copyTicks;
        NvU64 unmapTicks;
        NvU64 transferTicks;
        NvU32 path;
        bool verification;
    } timing_ = {};
    struct Statistics
    {
        NvU64 count = 0;
        NvU64 accepted = 0;
        NvU64 staging = 0;
        NvU64 readback = 0;
        NvU64 map = 0;
        NvU64 copy = 0;
        NvU64 transfer = 0;
        NvU64 upload = 0;
        NvU64 total = 0;
        NvU64 acceptedUpload = 0;
        NvU64 acceptedTotal = 0;
    } statistics_[2];
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SwapChainD3D12, GetSwapChainD3D12);

} // namespace nv::iddcx
