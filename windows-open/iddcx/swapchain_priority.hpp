#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d12.h>

namespace nv::iddcx {

HRESULT ReadRealtimeGpuPriority(bool* enabled) noexcept;

void TrySetRealtimeGpuPriority(
    IDDCX_SWAPCHAIN swapChain, IUnknown* device, PCSTR api) noexcept;

HRESULT CreateSourceCopyQueue(
    ID3D12Device* device, bool realtime, ID3D12CommandQueue** queue) noexcept;

class SwapChainThreadScheduling final
{
public:
    explicit SwapChainThreadScheduling(PCSTR api) noexcept;
    ~SwapChainThreadScheduling() noexcept;
    SwapChainThreadScheduling(const SwapChainThreadScheduling&) = delete;
    SwapChainThreadScheduling& operator=(const SwapChainThreadScheduling&) = delete;

private:
    PCSTR api_;
    HANDLE registration_ = nullptr;
};

} // namespace nv::iddcx
