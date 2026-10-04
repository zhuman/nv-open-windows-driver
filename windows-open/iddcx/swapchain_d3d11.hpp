#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wil/com.h>
#include <wil/resource.h>

namespace nv::iddcx {

class Output;
class SwapChainD3D11;

struct SwapChainFailureWorkContext final
{
    SwapChainD3D11* owner;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
    SwapChainFailureWorkContext,
    GetSwapChainFailureWorkContext);

class SwapChainD3D11 final
{
public:
    static NTSTATUS Create(
        Output& output,
        const IDARG_IN_SETSWAPCHAIN& arguments,
        SwapChainD3D11** swapChain) noexcept;
    ~SwapChainD3D11() noexcept;
    HRESULT StartProcessing() noexcept;
    void StopAndDelete() noexcept;

private:
    SwapChainD3D11(
        Output& output,
        IDDCX_SWAPCHAIN handle,
        HANDLE nextSurfaceEvent) noexcept;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
    static EVT_WDF_WORKITEM FailureCleanup;
    static DWORD WINAPI ThreadEntry(void* context) noexcept;
    HRESULT Initialize(LUID adapterLuid) noexcept;
    HRESULT CreateDevice(LUID adapterLuid) noexcept;
    HRESULT CopyFrame(IDXGIResource* surface) noexcept;
    void Process() noexcept;
    void Stop() noexcept;
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    Output* output_;
    IDDCX_SWAPCHAIN handle_;
    HANDLE nextSurfaceEvent_;
    wil::com_ptr_nothrow<IDXGIDevice> device_;
    wil::com_ptr_nothrow<ID3D11Texture2D> readbackTexture_;
    wil::unique_handle stopEvent_;
    wil::unique_handle thread_;
    wil::srwlock stateLock_;
    WDFWORKITEM failureWorkItem_;
    bool deletionStarted_;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SwapChainD3D11, GetSwapChainD3D11);

} // namespace nv::iddcx
