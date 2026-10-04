#include "swapchain_d3d11.hpp"
#include "adapter.hpp"
#include "display_modes.hpp"
#include "output.hpp"
#include "trace.h"

#include <cstring>
#include <new>
#include <utility>
#include <dxgi1_4.h>

#include "swapchain_d3d11.tmh"

#ifndef NV_IDDCX_DROP_SWAPCHAIN_FRAMES
#define NV_IDDCX_DROP_SWAPCHAIN_FRAMES 1
#endif

namespace nv::iddcx {

SwapChainD3D11::SwapChainD3D11(
        Output& output,
        IDDCX_SWAPCHAIN handle,
        HANDLE nextSurfaceEvent) noexcept
        : output_(&output),
            handle_(handle),
            nextSurfaceEvent_(nextSurfaceEvent),
            device_(),
            readbackTexture_(),
            stopEvent_(),
            thread_(),
            stateLock_(),
            failureWorkItem_(nullptr),
            deletionStarted_(false)
{
}

SwapChainD3D11::~SwapChainD3D11() noexcept
{
    // WDF cleanup is the final lifetime gate, so no worker may survive it.
    Stop();
    Output* output = nullptr;
    {
        auto lock = stateLock_.lock_exclusive();
        output = output_;
        output_ = nullptr;
        deletionStarted_ = true;
    }
    if (output != nullptr)
    {
        output->OnSwapChainDestroyed(this);
    }

    // The device-parented work item is independent of the swapchain hierarchy.
    if (failureWorkItem_ != nullptr)
    {
        GetSwapChainFailureWorkContext(failureWorkItem_)->owner = nullptr;
        WdfObjectDelete(failureWorkItem_);
        failureWorkItem_ = nullptr;
    }
}

NTSTATUS SwapChainD3D11::Create(
    Output& output,
    const IDARG_IN_SETSWAPCHAIN& arguments,
    SwapChainD3D11** swapChain) noexcept
{
    *swapChain = nullptr;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SwapChainD3D11);
    attributes.EvtCleanupCallback = Cleanup;
    void* context = nullptr;
    NTSTATUS status = WdfObjectAllocateContext(arguments.hSwapChain, &attributes, &context);
    if (!NT_SUCCESS(status))
    {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    // The swapchain owns raw WDF storage; ordinary heap allocation is disabled.
    auto* instance = ::new (context) SwapChainD3D11(
        output,
        arguments.hSwapChain,
        arguments.hNextSurfaceAvailable);
    const HRESULT result = instance->Initialize(arguments.RenderAdapterLuid);
    if (FAILED(result))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: initialization failed 0x%08X",
            result);
        WdfObjectDelete(arguments.hSwapChain);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    *swapChain = instance;
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AssignSwapChain: processing swapchain=%p",
        arguments.hSwapChain);
    return STATUS_SUCCESS;
}

HRESULT SwapChainD3D11::Initialize(LUID adapterLuid) noexcept
{
#if !NV_IDDCX_DROP_SWAPCHAIN_FRAMES
    HRESULT result = CreateDevice(adapterLuid);
    if (FAILED(result))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: D3D device creation failed 0x%08X",
            result);
        return result;
    }
    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = device_.get();
    result = IddCxSwapChainSetDevice(handle_, &setDevice);
    if (FAILED(result))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "AssignSwapChain: set D3D device failed 0x%08X",
            result);
        return result;
    }
#else
    UNREFERENCED_PARAMETER(adapterLuid);
#endif

    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, FailureCleanup);
    WDF_OBJECT_ATTRIBUTES workItemAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &workItemAttributes,
        SwapChainFailureWorkContext);
    workItemAttributes.ParentObject = output_->Owner().Device();
    const NTSTATUS workItemStatus = WdfWorkItemCreate(
        &workItemConfig,
        &workItemAttributes,
        &failureWorkItem_);
    if (!NT_SUCCESS(workItemStatus))
    {
        return HRESULT_FROM_NT(workItemStatus);
    }
    GetSwapChainFailureWorkContext(failureWorkItem_)->owner = this;

    wil::unique_handle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    stopEvent_ = std::move(stopEvent);
    return S_OK;
}

HRESULT SwapChainD3D11::StartProcessing() noexcept
{
    wil::unique_handle thread(CreateThread(
        nullptr,
        0,
        ThreadEntry,
        this,
        0,
        nullptr));
    if (!thread)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    thread_ = std::move(thread);
    return S_OK;
}

HRESULT SwapChainD3D11::CreateDevice(LUID luid) noexcept
{
    wil::com_ptr_nothrow<IDXGIFactory4> factory;
    HRESULT result = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void());
    if (FAILED(result))
    {
        return result;
    }
    wil::com_ptr_nothrow<IDXGIAdapter> adapter;
    result = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), adapter.put_void());
    if (FAILED(result))
    {
        return result;
    }
    wil::com_ptr_nothrow<ID3D11Device> device;
    result = D3D11CreateDevice(
        adapter.get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        device.put(),
        nullptr,
        nullptr);
    if (FAILED(result))
    {
        return result;
    }
    return device.query_to(device_.put());
}

void SwapChainD3D11::StopAndDelete() noexcept
{
    // Only monitor callback/cleanup paths delete; the worker never self-joins.
    Stop();
    {
        auto lock = stateLock_.lock_exclusive();
        if (deletionStarted_)
        {
            return;
        }
        deletionStarted_ = true;
        if (output_ != nullptr)
        {
            output_->OnSwapChainDestroyed(this);
        }
        output_ = nullptr;
    }
    WdfObjectDelete(handle_);
}

_Use_decl_annotations_
void SwapChainD3D11::FailureCleanup(WDFWORKITEM workItem)
{
    SwapChainD3D11* owner = GetSwapChainFailureWorkContext(workItem)->owner;
    if (owner != nullptr)
    {
        const IDDCX_SWAPCHAIN handle = owner->handle_;
        {
            auto lock = owner->stateLock_.lock_exclusive();
            owner->failureWorkItem_ = nullptr;
        }
        GetSwapChainFailureWorkContext(workItem)->owner = nullptr;
        owner->StopAndDelete();
        WdfObjectDelete(workItem);

        // Dropping the worker's reference may synchronously invoke Cleanup.
        WdfObjectDereference(handle);
    }
}

void SwapChainD3D11::Stop() noexcept
{
    // Snapshot wait handles under the SRW lock, then join without holding it.
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
    {
        auto lock = stateLock_.lock_shared();
        stopEvent = stopEvent_.get();
        thread = thread_.get();
    }
    if (stopEvent != nullptr)
    {
        SetEvent(stopEvent);
    }
    if (thread != nullptr)
    {
        WaitForSingleObject(thread, INFINITE);
    }
    auto lock = stateLock_.lock_exclusive();
    thread_.reset();
    stopEvent_.reset();
    nextSurfaceEvent_ = nullptr;
    readbackTexture_.reset();
    device_.reset();
}

DWORD WINAPI SwapChainD3D11::ThreadEntry(void* context) noexcept
{
    static_cast<SwapChainD3D11*>(context)->Process();
    return 0;
}

void SwapChainD3D11::Process() noexcept
{
    HANDLE stopEvent = nullptr;
    HANDLE nextSurfaceEvent = nullptr;
    {
        auto lock = stateLock_.lock_shared();
        stopEvent = stopEvent_.get();
        nextSurfaceEvent = nextSurfaceEvent_;
    }
    const HANDLE waitHandles[] = { nextSurfaceEvent, stopEvent };
    HRESULT result = S_OK;
    ULONG frameCount = 0;
    bool failed = false;
    for (;;)
    {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        result = IddCxSwapChainReleaseAndAcquireBuffer(handle_, &buffer);
        if (result == E_PENDING)
        {
            const DWORD waitResult = WaitForMultipleObjects(
                ARRAYSIZE(waitHandles),
                waitHandles,
                FALSE,
                INFINITE);
            if (waitResult == WAIT_OBJECT_0)
            {
                continue;
            }
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                break;
            }
            result = HRESULT_FROM_WIN32(GetLastError());
            failed = true;
            break;
        }
        if (FAILED(result))
        {
            failed = true;
            break;
        }
        if (buffer.MetaData.pSurface != nullptr)
        {
            // Acquisition transfers one resource reference; release it before finishing.
            wil::com_ptr_nothrow<IDXGIResource> surface;
            surface.attach(buffer.MetaData.pSurface);

#if !NV_IDDCX_DROP_SWAPCHAIN_FRAMES
            // Keep mode and scanout ownership stable through both transfer stages.
            auto outputLock = output_->LockFrameStateShared();
            result = CopyFrame(surface.get());
            if (FAILED(result))
            {
                failed = true;
                break;
            }
            const auto* mode = display_modes::FindDisplayMode(
                output_->DisplayState(), output_->ActiveModeIndex());
            if (mode == nullptr)
            {
                result = E_UNEXPECTED;
                failed = true;
                break;
            }
            NV_KMS_WINDOWS_LAYER layer = {};
            layer.surface = output_->ScanoutSurface();
            layer.sourceWidth = layer.destinationWidth = mode->hVisible;
            layer.sourceHeight = layer.destinationHeight = mode->vVisible;
            layer.opacity = 255;
            const NV_STATUS presentStatus = NvKmsWindowsPresentLayers(
                output_->AssignedHead(), &layer, 1);
            if (presentStatus != NV_OK && presentStatus != NV_ERR_IN_USE)
            {
                result = E_FAIL;
                failed = true;
                break;
            }
#endif
        }
        result = IddCxSwapChainFinishedProcessingFrame(handle_);
        if (FAILED(result))
        {
            failed = true;
            break;
        }
        ++frameCount;
    }
    TraceEvents(
        failed ? TRACE_LEVEL_ERROR : TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "Swap-chain processor stopped: result=0x%08X frames=%u",
        result,
        frameCount);
    if (failed && failureWorkItem_ != nullptr)
    {
        // Keep the swapchain context alive until device-parented cleanup runs.
        WdfObjectReference(handle_);
        WdfWorkItemEnqueue(failureWorkItem_);
    }
}

HRESULT SwapChainD3D11::CopyFrame(IDXGIResource* surface) noexcept
{
    if (output_->ScanoutPixels() == nullptr || output_->ScanoutPitch() == 0)
    {
        return E_UNEXPECTED;
    }
    const auto* mode = display_modes::FindDisplayMode(
        output_->DisplayState(), output_->ActiveModeIndex());
    if (mode == nullptr)
    {
        return E_UNEXPECTED;
    }

    wil::com_ptr_nothrow<ID3D11Texture2D> source;
    HRESULT result = surface->QueryInterface(__uuidof(ID3D11Texture2D), source.put_void());
    if (FAILED(result))
    {
        return result;
    }
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Width != mode->hVisible || sourceDesc.Height != mode->vVisible ||
        (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
         sourceDesc.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
         sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
    {
        return DXGI_ERROR_UNSUPPORTED;
    }

    wil::com_ptr_nothrow<ID3D11Device> device;
    source->GetDevice(device.put());
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    ID3D11Texture2D* mappedTexture = source.get();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    result = context->Map(mappedTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        bool replace = !readbackTexture_;
        if (!replace)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            readbackTexture_->GetDesc(&desc);
            replace = desc.Width != sourceDesc.Width || desc.Height != sourceDesc.Height ||
                desc.Format != sourceDesc.Format;
        }
        if (replace)
        {
            D3D11_TEXTURE2D_DESC desc = sourceDesc;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            result = device->CreateTexture2D(&desc, nullptr, readbackTexture_.put());
            if (FAILED(result))
            {
                return result;
            }
        }
        mappedTexture = readbackTexture_.get();
        context->CopyResource(mappedTexture, source.get());
        result = context->Map(mappedTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result))
        {
            return result;
        }
    }

    // Mapping must be undone on every pitch/format/copy exit.
    auto unmap = wil::scope_exit([&]() noexcept { context->Unmap(mappedTexture, 0); });
    auto* destination = static_cast<NvU8*>(output_->ScanoutPixels());
    const auto* sourceBytes = static_cast<const NvU8*>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(mode->hVisible) * 4;
    if (output_->ScanoutPitch() < rowBytes)
    {
        return E_UNEXPECTED;
    }
    const bool rgba = sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (NvU32 y = 0; y < mode->vVisible; ++y)
    {
        NvU8* destinationRow = destination + output_->ScanoutPitch() * y;
        const NvU8* sourceRow = sourceBytes + mapped.RowPitch * y;
        if (!rgba)
        {
            std::memcpy(destinationRow, sourceRow, rowBytes);
            continue;
        }
        for (NvU32 x = 0; x < mode->hVisible; ++x)
        {
            destinationRow[x * 4] = sourceRow[x * 4 + 2];
            destinationRow[x * 4 + 1] = sourceRow[x * 4 + 1];
            destinationRow[x * 4 + 2] = sourceRow[x * 4];
            destinationRow[x * 4 + 3] = 0xff;
        }
    }
    return S_OK;
}

_Use_decl_annotations_
void SwapChainD3D11::Cleanup(WDFOBJECT object)
{
    GetSwapChainD3D11(object)->~SwapChainD3D11();
}

} // namespace nv::iddcx
