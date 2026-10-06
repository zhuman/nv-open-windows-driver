#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include "nvkms_windows.h"
#include "swapchain_damage.hpp"

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
    HRESULT CopyFrame(wil::com_ptr_nothrow<IDXGIResource>& surface) noexcept;
    HRESULT CopyFrame(const IDDCX_SYSTEM_BUFFER_INFO& buffer) noexcept;
    HRESULT VerifyReadback(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* source,
        const D3D11_TEXTURE2D_DESC& sourceDesc) noexcept;
    HRESULT FinishFrame() noexcept;
    void UpdateHdrMetadata(const IDDCX_METADATA2& metadata) noexcept;
    void Process() noexcept;
    void Stop() noexcept;
    NvU64 Microseconds(NvU64 ticks) const noexcept;
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    Output* output_;
    IDDCX_SWAPCHAIN handle_;
    HANDLE nextSurfaceEvent_;
    wil::com_ptr_nothrow<IDXGIDevice> device_;
    wil::com_ptr_nothrow<ID3D11Texture2D> readbackTexture_;
    wil::com_ptr_nothrow<ID3D11Texture2D> defaultReadbackTexture_;
    wil::unique_handle stopEvent_;
    wil::unique_handle thread_;
    wil::srwlock stateLock_;
    WDFWORKITEM failureWorkItem_;
    NV_KMS_WINDOWS_HDR_METADATA currentHdrMetadata_;
    bool hasCurrentHdrMetadata_;
    bool useBuffer2_;
    bool useSystemMemory_;
    bool deletionStarted_;
    bool readFromSubresourceSupported_ = false;
    bool defaultReadbackRejected_ = false;
    bool readbackVerified_ = false;
    NvU32 readbackMode_ = 0;
    NvU64 textureFrameCount_ = 0;
    SwapChainDamage damage_;
    LARGE_INTEGER counterFrequency_ = {};
    struct
    {
        NvU64 stagingTicks;
        NvU64 readbackTicks;
        NvU64 mapTicks;
        NvU64 copyTicks;
        NvU64 unmapTicks;
        NvU64 finishTicks;
        NvU64 transferTicks;
        UINT width;
        UINT height;
        DXGI_FORMAT format;
        NvU32 path;
        bool readFromSubresourceRequested;
        bool verification;
        bool frameQueued;
    } copyTiming_ = {};
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SwapChainD3D11, GetSwapChainD3D11);

} // namespace nv::iddcx
