#include "swapchain_d3d12.hpp"
#include "adapter.hpp"
#include "device.hpp"
#include "driver_parameters.hpp"
#include "swapchain_pixels.hpp"
#include "trace.h"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <cstring>
#include <new>
#include <utility>

#include "swapchain_d3d12.tmh"

namespace nv::iddcx {
namespace {

NvU64 PerformanceCounter() noexcept
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<NvU64>(counter.QuadPart);
}

D3D12_RESOURCE_DESC BufferDescription(NvU64 size, D3D12_RESOURCE_FLAGS flags) noexcept
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

HRESULT CheckDebugMessages(ID3D12Device* device) noexcept
{
    wil::com_ptr_nothrow<ID3D12InfoQueue> info;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(info.put()))))
    {
        return S_OK; // The normal, non-debug device does not expose an info queue.
    }
    HRESULT result = S_OK;
    const UINT64 count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index)
    {
        SIZE_T size = 0;
        HRESULT messageResult = info->GetMessage(index, nullptr, &size);
        if (FAILED(messageResult))
        {
            return messageResult;
        }
        auto* message = static_cast<D3D12_MESSAGE*>(
            HeapAlloc(GetProcessHeap(), 0, size));
        if (message == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        auto freeMessage = wil::scope_exit([&]() noexcept {
            HeapFree(GetProcessHeap(), 0, message);
        });
        messageResult = info->GetMessage(index, message, &size);
        if (FAILED(messageResult))
        {
            return messageResult;
        }
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "D3D12 diagnostic: severity=%u id=%u %s",
                message->Severity, message->ID, message->pDescription);
        }
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            result = E_FAIL;
        }
    }
    info->ClearStoredMessages();
    return result;
}

} // namespace

SwapChainD3D12::SwapChainD3D12(
    Output& output, IDDCX_SWAPCHAIN handle, HANDLE nextSurfaceEvent,
    NvU32 mode) noexcept
    : output_(&output), handle_(handle), nextSurfaceEvent_(nextSurfaceEvent), mode_(mode)
{
}

SwapChainD3D12::~SwapChainD3D12() noexcept
{
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
    if (failureWorkItem_ != nullptr)
    {
        GetSwapChainD3D12FailureContext(failureWorkItem_)->owner = nullptr;
        WdfObjectDelete(failureWorkItem_);
        failureWorkItem_ = nullptr;
    }
}

NTSTATUS SwapChainD3D12::Create(
    Output& output, const IDARG_IN_SETSWAPCHAIN& arguments,
    NvU32 mode, SwapChainD3D12** swapChain) noexcept
{
    *swapChain = nullptr;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SwapChainD3D12);
    attributes.EvtCleanupCallback = Cleanup;
    void* storage = nullptr;
    const NTSTATUS status = WdfObjectAllocateContext(
        arguments.hSwapChain, &attributes, &storage);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 swapchain context allocation: %!STATUS!", status);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    auto* instance = ::new (storage) SwapChainD3D12(
        output, arguments.hSwapChain, arguments.hNextSurfaceAvailable, mode);
    const HRESULT result = instance->Initialize(arguments.RenderAdapterLuid);
    if (FAILED(result))
    {
        if (instance->device_)
        {
            CheckDebugMessages(instance->device_.get());
        }
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 initialization failed: result=0x%08X mode=%u; using D3D11 baseline",
            result, mode);
        instance->Stop();
        // Keep the inert WDF context until normal cleanup; do not delete the
        // shared swapchain object before the D3D11 fallback can bind it.
        instance->output_ = nullptr;
        return STATUS_NOT_SUPPORTED;
    }
    *swapChain = instance;
    return STATUS_SUCCESS;
}

HRESULT SwapChainD3D12::Initialize(LUID adapterLuid) noexcept
{
    QueryPerformanceFrequency(&counterFrequency_);
    const HRESULT damageResult = damage_.Initialize(handle_, 2);
    if (FAILED(damageResult))
    {
        return damageResult;
    }
    if (!GetDeviceContext(output_->Owner().Device())->d3d12SwapChainSupported ||
        !IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainSetDevice2) ||
        !IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2))
    {
        return E_NOTIMPL;
    }
    ULONG debugEnabled = 0;
    const NTSTATUS debugStatus = QueryDriverParameterDword(L"D3D12Debug", &debugEnabled);
    if (!NT_SUCCESS(debugStatus) && !DriverParameterAbsent(debugStatus))
    {
        return HRESULT_FROM_NT(debugStatus);
    }
    if (debugEnabled != 0)
    {
        wil::com_ptr_nothrow<ID3D12Debug> debug;
        const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(debug.put()));
        if (FAILED(result))
        {
            return result;
        }
        debug->EnableDebugLayer();
    }
    wil::com_ptr_nothrow<IDXGIFactory4> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(result))
    {
        return result;
    }
    wil::com_ptr_nothrow<IDXGIAdapter> adapter;
    result = factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(adapter.put()));
    if (FAILED(result))
    {
        return result;
    }
    result = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(device_.put()));
    if (FAILED(result))
    {
        return result;
    }
    if (debugEnabled != 0)
    {
        wil::com_ptr_nothrow<ID3D12InfoQueue> diagnostics;
        result = device_->QueryInterface(IID_PPV_ARGS(diagnostics.put()));
        if (FAILED(result))
        {
            return result;
        }
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture = {};
    result = device_->CheckFeatureSupport(
        D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture));
    if (FAILED(result))
    {
        return result;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "D3D12 swapchain: mode=%u debug=%u LUID=%08X:%08X UMA=%u coherent=%u",
        mode_, debugEnabled, adapterLuid.HighPart, adapterLuid.LowPart,
        architecture.UMA, architecture.CacheCoherentUMA);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    result = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue_.put()));
    if (FAILED(result))
    {
        return result;
    }
    result = device_->CreateCommandAllocator(
        queueDesc.Type, IID_PPV_ARGS(allocator_.put()));
    if (FAILED(result))
    {
        return result;
    }
    result = device_->CreateCommandList(0, queueDesc.Type, allocator_.get(),
        nullptr, IID_PPV_ARGS(commands_.put()));
    if (FAILED(result))
    {
        return result;
    }
    result = commands_->Close();
    if (FAILED(result))
    {
        return result;
    }
    result = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put()));
    if (FAILED(result))
    {
        return result;
    }
    fenceEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!fenceEvent_ || !stopEvent_)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const NTSTATUS backStatus = output_->PrepareBackBuffer();
    if (!NT_SUCCESS(backStatus))
    {
        return HRESULT_FROM_NT(backStatus);
    }
    if (mode_ != 2)
    {
        auto guard = output_->LockFrameStateShared();
        for (NvU32 index = 0; index < 2; ++index)
        {
            result = ImportBuffer(index);
            if (FAILED(result))
            {
                return result;
            }
        }
    }
    WDF_WORKITEM_CONFIG config;
    WDF_WORKITEM_CONFIG_INIT(&config, FailureCleanup);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SwapChainD3D12FailureContext);
    attributes.ParentObject = output_->Owner().Device();
    const NTSTATUS workStatus = WdfWorkItemCreate(
        &config, &attributes, &failureWorkItem_);
    if (!NT_SUCCESS(workStatus))
    {
        return HRESULT_FROM_NT(workStatus);
    }
    GetSwapChainD3D12FailureContext(failureWorkItem_)->owner = this;
    IDARG_IN_SWAPCHAINSETDEVICE2 setDevice = {};
    setDevice.Type = IDDCX_SWAPCHAIN_DEVICE_TYPE_D3D12;
    setDevice.Device.pD3d12Device = device_.get();
    return IddCxSwapChainSetDevice2(handle_, &setDevice);
}

HRESULT SwapChainD3D12::ImportBuffer(NvU32 index) noexcept
{
    auto& buffer = buffers_[index];
    NV_KMS_WINDOWS_SURFACE* surface = index == output_->BackBufferIndex()
        ? output_->BackSurface() : output_->ScanoutSurface();
    void* pixels = nullptr;
    NvU64 size = 0;
    const NV_STATUS status = NvKmsWindowsGetScanoutBuffer(surface, &pixels, &size);
    if (status != NV_OK)
    {
        return E_UNEXPECTED;
    }
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(pixels, &memory, sizeof(memory)) == 0)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "D3D12 WDF buffer: slot=%u pixels=%p base=%p region=%llu size=%llu protect=0x%08X type=0x%08X pitch=%u",
        index, pixels, memory.AllocationBase, static_cast<NvU64>(memory.RegionSize),
        size, memory.Protect, memory.Type, output_->ScanoutPitch());
    // Do not import a larger private arena containing an unrelated allocation.
    if (pixels != memory.AllocationBase || memory.State != MEM_COMMIT ||
        size > memory.RegionSize ||
        output_->ScanoutPitch() % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
    }
    HRESULT result = device_->OpenExistingHeapFromAddress(
        pixels, IID_PPV_ARGS(buffer.heap.put()));
    if (FAILED(result))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 WDF heap import: slot=%u result=0x%08X", index, result);
        return result;
    }
    const D3D12_HEAP_DESC heapDesc = buffer.heap->GetDesc();
    const auto desc = BufferDescription(size,
        (heapDesc.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) != 0
            ? D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER : D3D12_RESOURCE_FLAG_NONE);
    const auto allocation = device_->GetResourceAllocationInfo(0, 1, &desc);
    if (allocation.SizeInBytes > size || allocation.SizeInBytes > heapDesc.SizeInBytes)
    {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    result = device_->CreatePlacedResource(buffer.heap.get(), 0, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(buffer.resource.put()));
    if (FAILED(result))
    {
        return result;
    }
    buffer.pixels = pixels;
    buffer.size = size;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "D3D12 WDF heap imported: slot=%u heapSize=%llu flags=0x%08X CPUPage=%u pool=%u",
        index, heapDesc.SizeInBytes, heapDesc.Flags,
        heapDesc.Properties.CPUPageProperty, heapDesc.Properties.MemoryPoolPreference);
    return CheckDebugMessages(device_.get());
}

HRESULT SwapChainD3D12::StartProcessing() noexcept
{
    auto lock = stateLock_.lock_exclusive();
    wil::unique_handle thread(CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr));
    if (!thread)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    thread_ = std::move(thread);
    return S_OK;
}

_Use_decl_annotations_
void SwapChainD3D12::Cleanup(WDFOBJECT object)
{
    GetSwapChainD3D12(object)->~SwapChainD3D12();
}

_Use_decl_annotations_
void SwapChainD3D12::FailureCleanup(WDFWORKITEM workItem)
{
    SwapChainD3D12* owner = GetSwapChainD3D12FailureContext(workItem)->owner;
    if (owner != nullptr)
    {
        const IDDCX_SWAPCHAIN handle = owner->handle_;
        {
            auto lock = owner->stateLock_.lock_exclusive();
            owner->failureWorkItem_ = nullptr;
        }
        GetSwapChainD3D12FailureContext(workItem)->owner = nullptr;
        owner->StopAndDelete();
        WdfObjectDelete(workItem);
        WdfObjectDereference(handle);
    }
}

void SwapChainD3D12::StopAndDelete() noexcept
{
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

void SwapChainD3D12::Stop() noexcept
{
    HANDLE thread = nullptr;
    {
        auto lock = stateLock_.lock_shared();
        if (stopEvent_)
        {
            SetEvent(stopEvent_.get());
        }
        thread = thread_.get();
    }
    if (thread != nullptr)
    {
        WaitForSingleObject(thread, INFINITE);
    }
    auto lock = stateLock_.lock_exclusive();
    UnmapFrame();
    for (auto& buffer : buffers_)
    {
        buffer.resource.reset();
        buffer.heap.reset();
        buffer.pixels = nullptr;
    }
    readback_.reset();
    commands_.reset();
    allocator_.reset();
    queue_.reset();
    fence_.reset();
    device_.reset();
    thread_.reset();
    stopEvent_.reset();
    fenceEvent_.reset();
    nextSurfaceEvent_ = nullptr;
}

DWORD WINAPI SwapChainD3D12::ThreadEntry(void* context) noexcept
{
    static_cast<SwapChainD3D12*>(context)->Process();
    return 0;
}

HRESULT SwapChainD3D12::WaitForGpu() noexcept
{
    HRESULT result = fence_->SetEventOnCompletion(fenceValue_, fenceEvent_.get());
    if (FAILED(result))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 fence event failed: result=0x%08X; polling until quiesced", result);
    }
    NvU32 waits = 0;
    for (;;)
    {
        const NvU64 completed = fence_->GetCompletedValue();
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed) || completed == ~NvU64{0})
        {
            return FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED;
        }
        if (completed >= fenceValue_)
        {
            return S_OK;
        }
        if (SUCCEEDED(result))
        {
            const DWORD wait = WaitForSingleObject(fenceEvent_.get(), 1000);
            if (wait == WAIT_FAILED)
            {
                result = HRESULT_FROM_WIN32(GetLastError());
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "D3D12 fence wait failed: result=0x%08X; polling until quiesced", result);
            }
        }
        else
        {
            Sleep(1000);
        }
        if (++waits == 2 || waits % 10 == 0)
        {
            // Cancellation cannot free a common buffer still targeted by the GPU.
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "D3D12 fence stalled: seconds=%u value=%llu; retaining WDF backing memory",
                waits, fenceValue_);
        }
    }
}

void SwapChainD3D12::UnmapFrame() noexcept
{
    if (mappedResource_ != nullptr)
    {
        const NvU64 start = PerformanceCounter();
        const D3D12_RANGE noWrites = {0, 0};
        mappedResource_->Unmap(0, &noWrites);
        mappedResource_ = nullptr;
        timing_.unmapTicks += PerformanceCounter() - start;
    }
}

HRESULT SwapChainD3D12::CopyFrame(ID3D12Resource* source, NvU64 frameNumber) noexcept
{
    const NvU64 start = PerformanceCounter();
    auto recordTransfer = wil::scope_exit([&]() noexcept {
        timing_.transferTicks = PerformanceCounter() - start;
    });
    if (mode_ == 3 && frameNumber % 240 == 0)
    {
        // Revalidate both slots when ownership switches from CPU back to GPU.
        for (auto& buffer : buffers_)
        {
            buffer.verified = false;
            buffer.partialVerified = false;
        }
    }
    const auto desc = source->GetDesc();
    const auto* mode = display_modes::FindDisplayMode(
        output_->DisplayState(), output_->ActiveModeIndex());
    if (mode == nullptr || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width != mode->hVisible || desc.Height != mode->vVisible ||
        desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1)
    {
        return E_INVALIDARG;
    }
    if (!readback_ || desc.Width != readbackSourceDesc_.Width ||
        desc.Height != readbackSourceDesc_.Height || desc.Format != readbackSourceDesc_.Format)
    {
        UINT rows = 0;
        UINT64 rowBytes = 0;
        device_->GetCopyableFootprints(
            &desc, 0, 1, 0, &readbackFootprint_, &rows, &rowBytes, &readbackSize_);
        if (rows != desc.Height || readbackSize_ == ~NvU64{0} ||
            (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
             desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
             desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
             desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
             desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
        {
            return DXGI_ERROR_UNSUPPORTED;
        }
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        const auto bufferDesc = BufferDescription(readbackSize_, D3D12_RESOURCE_FLAG_NONE);
        readback_.reset();
        HRESULT result = device_->CreateCommittedResource(&properties,
            D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(readback_.put()));
        if (FAILED(result))
        {
            return result;
        }
        readbackSourceDesc_ = desc;
    }
    const bool matchingFormat =
        output_->ScanoutFormat() == NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16
            ? desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
            : desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
              desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
              desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    if (!matchingFormat && mode_ != 2 && !directRejected_)
    {
        directRejected_ = true;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 direct format mismatch: input=%u output=%u; using D3D12 CPU conversion",
            desc.Format, output_->ScanoutFormat());
    }
    const bool direct = !directRejected_ &&
        (mode_ == 1 || (mode_ == 3 && (frameNumber / 120) % 2 == 0));
    auto& buffer = buffers_[output_->BackBufferIndex()];
    if (direct && (buffer.pixels != output_->BackPixels() || !buffer.resource))
    {
        return E_UNEXPECTED;
    }
    timing_.path = direct ? 5 : 6;
    timing_.verification = direct && !buffer.verified;
    if (damage_.Rectangles().empty())
    {
        timing_.verification = false;
        return S_OK;
    }
    const bool partial = damage_.Enabled() &&
        (damage_.Rectangles().size() != 1 || damage_.Rectangles()[0].top != 0 ||
         static_cast<UINT>(damage_.Rectangles()[0].bottom) != desc.Height);
    if (direct && damage_.Enabled())
    {
        ++buffer.framesSinceVerification;
        timing_.verification = timing_.verification ||
            (partial && !buffer.partialVerified) || buffer.framesSinceVerification >= 120;
    }
    D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
    sourceLocation.pResource = source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION reference = {};
    reference.pResource = readback_.get();
    reference.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    reference.PlacedFootprint = readbackFootprint_;
    D3D12_TEXTURE_COPY_LOCATION target = reference;
    if (direct)
    {
        target.pResource = buffer.resource.get();
        target.PlacedFootprint.Offset = buffer.offset;
        target.PlacedFootprint.Footprint.RowPitch = output_->ScanoutPitch();
        const NvU64 extent = static_cast<NvU64>(output_->ScanoutPitch()) * desc.Height;
        if (extent > buffer.size)
        {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
    }
    HRESULT result = allocator_->Reset();
    if (FAILED(result))
    {
        return result;
    }
    result = commands_->Reset(allocator_.get(), nullptr);
    if (FAILED(result))
    {
        return result;
    }
    // COPY-queue accesses use COMMON-state implicit promotion and decay at
    // ExecuteCommandLists boundaries, including the imported linear buffer.
    if (damage_.Enabled())
    {
        for (const RECT& rectangle : damage_.Rectangles())
        {
            const D3D12_BOX box = {
                static_cast<UINT>(rectangle.left), static_cast<UINT>(rectangle.top), 0,
                static_cast<UINT>(rectangle.right), static_cast<UINT>(rectangle.bottom), 1};
            commands_->CopyTextureRegion(&target, box.left, box.top, 0, &sourceLocation, &box);
        }
    }
    else
    {
        commands_->CopyTextureRegion(&target, 0, 0, 0, &sourceLocation, nullptr);
    }
    if (timing_.verification)
    {
        commands_->CopyTextureRegion(&reference, 0, 0, 0, &sourceLocation, nullptr);
    }
    result = commands_->Close();
    if (FAILED(result))
    {
        return result;
    }
    ID3D12CommandList* lists[] = {commands_.get()};
    queue_->ExecuteCommandLists(ARRAYSIZE(lists), lists);
    const HRESULT signalResult = queue_->Signal(fence_.get(), ++fenceValue_);
    result = signalResult;
    if (FAILED(signalResult))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 fence signal failed: result=0x%08X; draining before fallback", signalResult);
        NvU32 retries = 0;
        while (FAILED(result) && SUCCEEDED(device_->GetDeviceRemovedReason()))
        {
            Sleep(100);
            result = queue_->Signal(fence_.get(), fenceValue_);
            if (++retries % 10 == 0)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "D3D12 drain signal retry: count=%u result=0x%08X", retries, result);
            }
        }
    }
    timing_.stagingTicks = PerformanceCounter() - start;
    const NvU64 waitStart = PerformanceCounter();
    // Even a stop request cannot release the source/backing allocation early.
    const HRESULT waitResult = WaitForGpu();
    timing_.readbackTicks = PerformanceCounter() - waitStart;
    if (FAILED(signalResult) || FAILED(waitResult))
    {
        return FAILED(signalResult) ? signalResult : waitResult;
    }
    result = CheckDebugMessages(device_.get());
    if (FAILED(result))
    {
        return result;
    }
    const NvU64 mapStart = PerformanceCounter();
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(direct ? buffer.size : readbackSize_)};
    ID3D12Resource* destination = target.pResource;
    result = destination->Map(0, &range, &mapped);
    timing_.mapTicks = PerformanceCounter() - mapStart;
    if (FAILED(result))
    {
        CheckDebugMessages(device_.get());
        return result;
    }
    mappedResource_ = destination; // Kept mapped through synchronous CE completion.
    if (timing_.verification)
    {
        void* referencePixels = nullptr;
        const D3D12_RANGE referenceRange = {0, static_cast<SIZE_T>(readbackSize_)};
        result = readback_->Map(0, &referenceRange, &referencePixels);
        if (FAILED(result))
        {
            return result;
        }
        auto unmapReference = wil::scope_exit([&]() noexcept {
            const D3D12_RANGE noWrites = {0, 0};
            readback_->Unmap(0, &noWrites);
        });
        const size_t rowBytes = static_cast<size_t>(desc.Width) *
            (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U);
        for (UINT y = 0; y < desc.Height; ++y)
        {
            const auto* originalRow = static_cast<const NvU8*>(buffer.pixels) +
                static_cast<size_t>(output_->ScanoutPitch()) * y;
            const auto* mappedRow = static_cast<const NvU8*>(mapped) +
                buffer.offset + static_cast<size_t>(output_->ScanoutPitch()) * y;
            const auto* referenceRow = static_cast<const NvU8*>(referencePixels) +
                readbackFootprint_.Offset +
                static_cast<size_t>(readbackFootprint_.Footprint.RowPitch) * y;
            if (std::memcmp(originalRow, referenceRow, rowBytes) != 0 ||
                std::memcmp(mappedRow, referenceRow, rowBytes) != 0)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "D3D12 WDF pixel verification FAILED: slot=%u row=%u",
                    output_->BackBufferIndex(), y);
                return HRESULT_FROM_WIN32(ERROR_CRC);
            }
        }
        buffer.verified = true;
        buffer.partialVerified = partial;
        buffer.framesSinceVerification = 0;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "D3D12 WDF pixel verification passed: slot=%u bytes=%llu original=%p mapped=%p pitch=%u format=%u partial=%u",
            output_->BackBufferIndex(), static_cast<NvU64>(rowBytes) * desc.Height,
            buffer.pixels, mapped, output_->ScanoutPitch(), desc.Format, partial);
    }
    if (!direct)
    {
        IDDCX_SYSTEM_BUFFER_INFO info = {};
        info.pBuffer = static_cast<NvU8*>(mapped) + readbackFootprint_.Offset;
        info.Width = static_cast<UINT>(desc.Width);
        info.Height = desc.Height;
        info.Pitch = readbackFootprint_.Footprint.RowPitch;
        info.Format = desc.Format;
        void* destinationPixels = output_->BackPixels();
        bool commonMapped = false;
        if (buffer.resource)
        {
            const NvU64 commonMapStart = PerformanceCounter();
            const D3D12_RANGE noReads = {0, 0};
            result = buffer.resource->Map(0, &noReads, &destinationPixels);
            timing_.mapTicks += PerformanceCounter() - commonMapStart;
            if (FAILED(result))
            {
                return result;
            }
            commonMapped = true;
        }
        auto unmapCommon = wil::scope_exit([&]() noexcept {
            if (commonMapped)
            {
                const NvU64 unmapStart = PerformanceCounter();
                const D3D12_RANGE written = {0, SUCCEEDED(result)
                    ? static_cast<SIZE_T>(output_->ScanoutPitch()) * desc.Height : 0};
                buffer.resource->Unmap(0, &written);
                timing_.unmapTicks += PerformanceCounter() - unmapStart;
            }
        });
        const NvU64 copyStart = PerformanceCounter();
        result = frame::CopyPixels(*output_, info, destinationPixels, damage_.Rectangles());
        timing_.copyTicks = PerformanceCounter() - copyStart;
    }
    return result;
}

void SwapChainD3D12::UpdateHdrMetadata(const IDDCX_METADATA2& metadata) noexcept
{
    if (output_->ColorSpace() != NV_KMS_WINDOWS_COLOR_SPACE_HDR10)
    {
        hasCurrentHdrMetadata_ = false;
        return;
    }
    if ((metadata.ValidFlags & IDDCX_METADATA2_VALID_FLAGS_HDR10METADATA) == 0)
    {
        return;
    }
    switch (metadata.Hdr10FrameMetaData.Type)
    {
    case IDDCX_HDR10_FRAME_METADATA_TYPE_DEFAULT:
        currentHdrMetadata_ = output_->DefaultHdrMetadata();
        hasCurrentHdrMetadata_ = output_->HasDefaultHdrMetadata();
        break;
    case IDDCX_HDR10_FRAME_METADATA_TYPE_NEW:
        frame::CopyHdrMetadata(metadata.Hdr10FrameMetaData.NewMetaData, &currentHdrMetadata_);
        hasCurrentHdrMetadata_ = true;
        break;
    case IDDCX_HDR10_FRAME_METADATA_TYPE_UNCHANGED:
        break;
    default:
        hasCurrentHdrMetadata_ = false;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "D3D12 invalid HDR metadata type=%u", metadata.Hdr10FrameMetaData.Type);
        break;
    }
}

NvU64 SwapChainD3D12::Microseconds(NvU64 ticks) const noexcept
{
    return ticks * 1000000 / static_cast<NvU64>(counterFrequency_.QuadPart);
}

void SwapChainD3D12::RecordTiming(
    NvU64 frameNumber, const NV_KMS_WINDOWS_PRESENT_TIMING& present,
    bool dropped, NvU64 totalTicks) noexcept
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "D3D12 timing us: frame=%llu path=%u staging=%llu gpuWait=%llu map=%llu cpuCopy=%llu unmap=%llu upload=%llu total=%llu dropped=%u verification=%u head=%u width=%llu height=%u format=%u transfer=%llu bytes=%llu regions=%u",
        frameNumber, timing_.path, Microseconds(timing_.stagingTicks),
        Microseconds(timing_.readbackTicks), Microseconds(timing_.mapTicks),
        Microseconds(timing_.copyTicks), Microseconds(timing_.unmapTicks),
        Microseconds(present.uploadTicks), Microseconds(totalTicks), dropped, timing_.verification,
        output_->AssignedHead(), readbackSourceDesc_.Width, readbackSourceDesc_.Height,
        readbackSourceDesc_.Format, Microseconds(timing_.transferTicks),
        present.uploadBytes, present.uploadRegions);
    if (timing_.verification || frameNumber < 8 ||
        (mode_ == 3 && frameNumber % 120 < 8))
    {
        return;
    }
    auto& stats = statistics_[timing_.path == 5 ? 0 : 1];
    ++stats.count;
    stats.staging += timing_.stagingTicks;
    stats.readback += timing_.readbackTicks;
    stats.map += timing_.mapTicks;
    stats.copy += timing_.copyTicks;
    stats.transfer += timing_.transferTicks;
    stats.upload += present.uploadTicks;
    stats.total += totalTicks;
    if (!dropped)
    {
        ++stats.accepted;
        stats.acceptedUpload += present.uploadTicks;
        stats.acceptedTotal += totalTicks;
    }
    if (stats.count % 60 == 0)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "D3D12 averages us: path=%u frames=%llu accepted=%llu staging=%llu gpuWait=%llu map=%llu cpuCopy=%llu upload=%llu total=%llu acceptedUpload=%llu acceptedTotal=%llu head=%u transfer=%llu",
            timing_.path, stats.count, stats.accepted,
            Microseconds(stats.staging / stats.count), Microseconds(stats.readback / stats.count),
            Microseconds(stats.map / stats.count), Microseconds(stats.copy / stats.count),
            Microseconds(stats.upload / stats.count), Microseconds(stats.total / stats.count),
            stats.accepted == 0 ? 0 : Microseconds(stats.acceptedUpload / stats.accepted),
            stats.accepted == 0 ? 0 : Microseconds(stats.acceptedTotal / stats.accepted),
            output_->AssignedHead(), Microseconds(stats.transfer / stats.count));
    }
}

void SwapChainD3D12::Process() noexcept
{
    {
        // StartProcessing publishes the thread handle before failures can queue cleanup.
        auto lock = stateLock_.lock_shared();
    }
    const HANDLE waits[] = {nextSurfaceEvent_, stopEvent_.get()};
    HRESULT result = S_OK;
    NvU64 frameNumber = 0;
    for (;;)
    {
        if (WaitForSingleObject(stopEvent_.get(), 0) == WAIT_OBJECT_0)
        {
            break;
        }
        timing_ = {};
        const NvU64 start = PerformanceCounter();
        IDARG_IN_RELEASEANDACQUIREBUFFER2 input = {};
        input.Size = IDD_STRUCTURE_SIZE(IDARG_IN_RELEASEANDACQUIREBUFFER2);
        input.pD3D12CommandQueue = queue_.get();
        IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = {};
        buffer.MetaData.Size = IDD_STRUCTURE_SIZE(IDDCX_METADATA2);
        result = IddCxSwapChainReleaseAndAcquireBuffer2(handle_, &input, &buffer);
        if (result == E_PENDING)
        {
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT)
            {
                continue;
            }
            if (wait == WAIT_OBJECT_0 + 1)
            {
                result = S_OK;
                break;
            }
            result = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (FAILED(result))
        {
            break;
        }
        wil::com_ptr_nothrow<ID3D12Resource> source;
        source.attach(buffer.MetaData.pD3D12Surface);
        if ((buffer.MetaData.ValidFlags & IDDCX_METADATA2_VALID_FLAGS_D3D12_SURFACE) == 0 ||
            !source || buffer.MetaData.HwProtectedSurface)
        {
            result = DXGI_ERROR_UNSUPPORTED;
            break;
        }
        auto guard = output_->LockFrameStateShared();
        auto unmap = wil::scope_exit([&]() noexcept { UnmapFrame(); });
        UpdateHdrMetadata(buffer.MetaData);
        const UINT bufferIndex = output_->BackBufferIndex();
        const auto* frameMode = display_modes::FindDisplayMode(
            output_->DisplayState(), output_->ActiveModeIndex());
        if (frameMode == nullptr)
        {
            result = E_UNEXPECTED;
            break;
        }
        result = damage_.Prepare(frameMode->hVisible, frameMode->vVisible,
            buffer.MetaData.PresentationFrameNumber, buffer.MetaData.DirtyRectCount, 0, bufferIndex);
        if (FAILED(result))
        {
            break;
        }
        result = CopyFrame(source.get(), frameNumber);
        if (FAILED(result))
        {
            break;
        }
        // Finish and reacquire only after all GPU reads of the source complete.
        source.reset();
        result = IddCxSwapChainFinishedProcessingFrame(handle_);
        if (FAILED(result))
        {
            break;
        }
        const auto* mode = display_modes::FindDisplayMode(
            output_->DisplayState(), output_->ActiveModeIndex());
        if (mode == nullptr)
        {
            result = E_UNEXPECTED;
            break;
        }
        NV_KMS_WINDOWS_LAYER layer = {};
        layer.surface = output_->BackSurface();
        layer.sourceWidth = layer.destinationWidth = mode->hVisible;
        layer.sourceHeight = layer.destinationHeight = mode->vVisible;
        layer.opacity = 255;
        layer.colorSpace = output_->ColorSpace();
        layer.hdrMetadata = currentHdrMetadata_;
        layer.hdrMetadataEnabled = hasCurrentHdrMetadata_ ? NV_TRUE : NV_FALSE;
        layer.colorTransform = &output_->ColorTransform();
        damage_.ApplyToLayer(layer);
        NV_KMS_WINDOWS_PRESENT_TIMING present = {};
        const NV_STATUS status = NvKmsWindowsPresentLayersTimed(
            output_->AssignedHead(), output_->DisplayState().displayHandle,
            &layer, 1, NV_TRUE, &present);
        if (status == NV_OK)
        {
            result = damage_.Commit(bufferIndex);
            if (FAILED(result))
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "D3D12 dirty-buffer commit failed: result=0x%08X buffer=%u", result, bufferIndex);
                break;
            }
            output_->CommitPresent();
        }
        else if (status != NV_ERR_IN_USE)
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "D3D12 CE/present failed: status=0x%08X", status);
            result = E_FAIL;
            break;
        }
        UnmapFrame();
        RecordTiming(frameNumber++, present, status == NV_ERR_IN_USE,
            PerformanceCounter() - start);
    }
    failed_ = FAILED(result);
    TraceEvents(failed_ ? TRACE_LEVEL_ERROR : TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "D3D12 processor stopped: result=0x%08X frames=%llu mode=%u",
        result, frameNumber, mode_);
    if (failed_ && failureWorkItem_ != nullptr &&
        WaitForSingleObject(stopEvent_.get(), 0) != WAIT_OBJECT_0)
    {
        WdfObjectReference(handle_);
        WdfWorkItemEnqueue(failureWorkItem_);
    }
}

} // namespace nv::iddcx
