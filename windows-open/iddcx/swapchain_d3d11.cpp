#include "swapchain_d3d11.hpp"
#include "adapter.hpp"
#include "display_modes.hpp"
#include "driver_parameters.hpp"
#include "output.hpp"
#include "swapchain_pixels.hpp"
#include "trace.h"

#include <cstring>
#include <new>
#include <utility>
#include <dxgi1_4.h>
#include <d3d11_3.h>

#include "swapchain_d3d11.tmh"

#ifndef NV_IDDCX_DROP_SWAPCHAIN_FRAMES
#define NV_IDDCX_DROP_SWAPCHAIN_FRAMES 0
#endif

namespace nv::iddcx {
namespace {

NvU64 PerformanceCounter() noexcept
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<NvU64>(counter.QuadPart);
}

} // namespace

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
            currentHdrMetadata_{},
            hasCurrentHdrMetadata_(false),
            useBuffer2_(false),
            useSystemMemory_(false),
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
    QueryPerformanceFrequency(&counterFrequency_);
    const HRESULT damageResult = damage_.Initialize(handle_, 2);
    if (FAILED(damageResult))
    {
        return damageResult;
    }
    useBuffer2_ = IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2);
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
    if (useBuffer2_ && IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainInSystemMemory))
    {
        BOOL inSystemMemory = FALSE;
        result = IddCxSwapChainInSystemMemory(handle_, &inSystemMemory);
        if (FAILED(result))
        {
            TraceEvents(
                TRACE_LEVEL_ERROR,
                TRACE_DRIVER,
                "AssignSwapChain: query system memory failed 0x%08X",
                result);
            return result;
        }
        useSystemMemory_ = inSystemMemory != FALSE;
    }
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "AssignSwapChain: buffer2=%u systemMemory=%u",
        useBuffer2_,
        useSystemMemory_);
    const NTSTATUS backBufferStatus = output_->PrepareBackBuffer();
    if (!NT_SUCCESS(backBufferStatus))
    {
        return HRESULT_FROM_NT(backBufferStatus);
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
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options = {};
    readFromSubresourceSupported_ = SUCCEEDED(device->CheckFeatureSupport(
        D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options))) &&
        options.MapOnDefaultTextures;
    DWORD mode = 0;
    const NTSTATUS modeStatus = QueryDriverParameterDword(L"D3D11ReadbackMode", &mode);
    if ((!NT_SUCCESS(modeStatus) && !DriverParameterAbsent(modeStatus)) || mode > 2)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Invalid D3D11ReadbackMode: status=%!STATUS! value=%u", modeStatus, mode);
        return NT_SUCCESS(modeStatus) ? E_INVALIDARG : HRESULT_FROM_NT(modeStatus);
    }
    DWORD modeSize = sizeof(mode);
    if (DriverParameterAbsent(modeStatus) && RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\NVIDIA Corporation\\nv-iddcx",
            L"ReadbackMode",
            RRF_RT_REG_DWORD,
            nullptr,
            &mode,
            &modeSize) == ERROR_SUCCESS && mode <= 2)
    {
        readbackMode_ = mode;
    }
    else if (NT_SUCCESS(modeStatus))
    {
        readbackMode_ = mode;
    }
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_DRIVER,
        "Readback configuration: mode=%u mapOnDefaultTextures=%u",
        readbackMode_,
        readFromSubresourceSupported_);
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
    defaultReadbackTexture_.reset();
    device_.reset();
}

DWORD WINAPI SwapChainD3D11::ThreadEntry(void* context) noexcept
{
    static_cast<SwapChainD3D11*>(context)->Process();
    return 0;
}

NvU64 SwapChainD3D11::Microseconds(NvU64 ticks) const noexcept
{
    return ticks * 1000000 / static_cast<NvU64>(counterFrequency_.QuadPart);
}

HRESULT SwapChainD3D11::FinishFrame() noexcept
{
    if (copyTiming_.frameQueued)
    {
        return S_OK;
    }
    const NvU64 start = PerformanceCounter();
    const HRESULT result = IddCxSwapChainFinishedProcessingFrame(handle_);
    copyTiming_.finishTicks += PerformanceCounter() - start;
    copyTiming_.frameQueued = SUCCEEDED(result);
    return result;
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
    NvU64 idleTicks = 0;
    for (;;)
    {
        copyTiming_ = {};
        NV_KMS_WINDOWS_PRESENT_TIMING presentTiming = {};
        NvU64 stateWaitTicks = 0;
        bool dropped = false;
        NvU32 bufferIndex = 0;
        NvU32 head = 0;
        const NvU64 frameStart = PerformanceCounter();
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        IDARG_IN_RELEASEANDACQUIREBUFFER2 input2 = {};
        IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer2 = {};
        if (useBuffer2_)
        {
            input2.Size = IDD_STRUCTURE_SIZE(IDARG_IN_RELEASEANDACQUIREBUFFER2);
            input2.AcquireSystemMemoryBuffer = useSystemMemory_;
            buffer2.MetaData.Size = IDD_STRUCTURE_SIZE(IDDCX_METADATA2);
            result = IddCxSwapChainReleaseAndAcquireBuffer2(
                handle_, &input2, &buffer2);
        }
        else
        {
            result = IddCxSwapChainReleaseAndAcquireBuffer(handle_, &buffer);
        }
        const NvU64 acquireTicks = PerformanceCounter() - frameStart;
        if (result == E_PENDING)
        {
            const NvU64 idleStart = PerformanceCounter();
            const DWORD waitResult = WaitForMultipleObjects(
                ARRAYSIZE(waitHandles),
                waitHandles,
                FALSE,
                16);
            idleTicks += PerformanceCounter() - idleStart;
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)
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
        const bool hasSystemBuffer = useBuffer2_ && useSystemMemory_ &&
            (buffer2.MetaData.ValidFlags &
                IDDCX_METADATA2_VALID_FLAGS_SYSTEMBUFFERINFO) != 0 &&
            buffer2.MetaData.SystemBufferInfo.pBuffer != nullptr;
        wil::com_ptr_nothrow<IDXGIResource> acquiredSurface;
        acquiredSurface.attach(useBuffer2_
            ? buffer2.MetaData.pSurface
            : buffer.MetaData.pSurface);
        if (hasSystemBuffer || acquiredSurface)
        {
#if !NV_IDDCX_DROP_SWAPCHAIN_FRAMES
            // Keep mode and scanout ownership stable through both transfer stages.
            const NvU64 stateWaitStart = PerformanceCounter();
            auto outputLock = output_->LockFrameStateShared();
            stateWaitTicks = PerformanceCounter() - stateWaitStart;
            bufferIndex = output_->BackBufferIndex();
            head = output_->AssignedHead();
            const auto* frameMode = display_modes::FindDisplayMode(
                output_->DisplayState(), output_->ActiveModeIndex());
            if (frameMode == nullptr)
            {
                result = E_UNEXPECTED;
                failed = true;
                break;
            }
            result = damage_.Prepare(frameMode->hVisible, frameMode->vVisible,
                useBuffer2_ ? buffer2.MetaData.PresentationFrameNumber : buffer.MetaData.PresentationFrameNumber,
                useBuffer2_ ? buffer2.MetaData.DirtyRectCount : buffer.MetaData.DirtyRectCount,
                useBuffer2_ ? 0 : buffer.MetaData.MoveRegionCount, bufferIndex);
            if (FAILED(result))
            {
                failed = true;
                break;
            }
            if (useBuffer2_)
            {
                UpdateHdrMetadata(buffer2.MetaData);
                if (frameCount == 0)
                {
                    TraceEvents(
                        TRACE_LEVEL_INFORMATION,
                        TRACE_DRIVER,
                        "Buffer2 first frame: surfaceColorSpace=%u SDRWhite=%u valid=0x%08X",
                        buffer2.MetaData.SurfaceColorSpace,
                        buffer2.MetaData.SdrWhiteLevel,
                        buffer2.MetaData.ValidFlags);
                }
            }
            const NvU64 transferStart = PerformanceCounter();
            result = hasSystemBuffer
                ? CopyFrame(buffer2.MetaData.SystemBufferInfo)
                : CopyFrame(acquiredSurface);
            copyTiming_.transferTicks = PerformanceCounter() - transferStart;
            if (SUCCEEDED(result))
            {
                acquiredSurface.reset();
                result = FinishFrame();
            }
            if (FAILED(result))
            {
                TraceEvents(
                    TRACE_LEVEL_ERROR,
                    TRACE_DRIVER,
                    "CopyFrame failed: result=0x%08X scanoutFormat=%u",
                    result,
                    output_->ScanoutFormat());
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
            layer.surface = output_->BackSurface();
            layer.sourceWidth = layer.destinationWidth = mode->hVisible;
            layer.sourceHeight = layer.destinationHeight = mode->vVisible;
            layer.opacity = 255;
            layer.colorSpace = output_->ColorSpace();
            layer.hdrMetadata = currentHdrMetadata_;
            layer.hdrMetadataEnabled = hasCurrentHdrMetadata_ ? NV_TRUE : NV_FALSE;
            layer.colorTransform = &output_->ColorTransform();
            damage_.ApplyToLayer(layer);
            const NV_STATUS presentStatus = NvKmsWindowsPresentLayersTimed(
                output_->AssignedHead(),
                output_->DisplayState().displayHandle,
                &layer,
                1,
                NV_TRUE,
                &presentTiming);
            dropped = presentStatus == NV_ERR_IN_USE;
            if (presentStatus == NV_OK)
            {
                result = damage_.Commit(bufferIndex);
                if (FAILED(result))
                {
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                        "Dirty-buffer commit failed: result=0x%08X buffer=%u", result, bufferIndex);
                    failed = true;
                    break;
                }
                output_->CommitPresent();
            }
            if (presentStatus != NV_OK && presentStatus != NV_ERR_IN_USE)
            {
                TraceEvents(
                    TRACE_LEVEL_ERROR,
                    TRACE_DRIVER,
                    "NvKmsWindowsPresentLayers: 0x%08X head=%u",
                    presentStatus,
                    output_->AssignedHead());
                result = E_FAIL;
                failed = true;
                break;
            }
#endif
        }
        acquiredSurface.reset();
        result = FinishFrame();
        if (FAILED(result))
        {
            TraceEvents(
                TRACE_LEVEL_ERROR,
                TRACE_DRIVER,
                "FinishedProcessingFrame failed: result=0x%08X",
                result);
            failed = true;
            break;
        }
        const UCHAR timingLevel = readbackMode_ == 2 || frameCount < 8 || frameCount % 60 == 0 ?
            TRACE_LEVEL_INFORMATION : TRACE_LEVEL_VERBOSE;
        TraceEvents(timingLevel, TRACE_DRIVER,
            "Frame upload: frame=%u bytes=%llu regions=%u",
            frameCount, presentTiming.uploadBytes, presentTiming.uploadRegions);
        TraceEvents(timingLevel,
            TRACE_DRIVER,
            "Frame timing us: frame=%u acquire=%llu stateWait=%llu staging=%llu readback=%llu map=%llu cpuCopy=%llu unmap=%llu kmsWait=%llu upload=%llu prepare=%llu submit=%llu finish=%llu total=%llu idle=%llu dropped=%u pending=%llu buffer=%u path=%u requested=%u verification=%u transfer=%llu head=%u width=%u height=%u format=%u",
            frameCount, Microseconds(acquireTicks), Microseconds(stateWaitTicks),
            Microseconds(copyTiming_.stagingTicks), Microseconds(copyTiming_.readbackTicks),
            Microseconds(copyTiming_.mapTicks), Microseconds(copyTiming_.copyTicks),
            Microseconds(copyTiming_.unmapTicks), Microseconds(presentTiming.lockTicks),
            Microseconds(presentTiming.uploadTicks), Microseconds(presentTiming.prepareTicks),
            Microseconds(presentTiming.submitTicks), Microseconds(copyTiming_.finishTicks),
            Microseconds(PerformanceCounter() - frameStart), Microseconds(idleTicks), dropped,
            Microseconds(presentTiming.pendingTicks), bufferIndex,
            copyTiming_.path, copyTiming_.readFromSubresourceRequested,
            copyTiming_.verification, Microseconds(copyTiming_.transferTicks),
            head, copyTiming_.width, copyTiming_.height, copyTiming_.format);
        idleTicks = 0;
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

void SwapChainD3D11::UpdateHdrMetadata(const IDDCX_METADATA2& metadata) noexcept
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
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "Frame HDR metadata: default available=%u",
            hasCurrentHdrMetadata_);
        break;
    case IDDCX_HDR10_FRAME_METADATA_TYPE_NEW:
        frame::CopyHdrMetadata(
            metadata.Hdr10FrameMetaData.NewMetaData,
            &currentHdrMetadata_);
        hasCurrentHdrMetadata_ = true;
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "Frame HDR metadata: new MaxCLL=%u MaxFALL=%u",
            currentHdrMetadata_.maxContentLightLevel,
            currentHdrMetadata_.maxFrameAverageLightLevel);
        break;
    case IDDCX_HDR10_FRAME_METADATA_TYPE_UNCHANGED:
        break;
    default:
        hasCurrentHdrMetadata_ = false;
        break;
    }
}

HRESULT SwapChainD3D11::CopyFrame(
    const IDDCX_SYSTEM_BUFFER_INFO& buffer) noexcept
{
    copyTiming_.width = buffer.Width;
    copyTiming_.height = buffer.Height;
    copyTiming_.format = buffer.Format;
    const NvU64 copyStart = PerformanceCounter();
    auto recordCopy = wil::scope_exit([&]() noexcept {
        copyTiming_.copyTicks += PerformanceCounter() - copyStart;
    });
    return frame::CopyPixels(*output_, buffer, output_->BackPixels(), damage_.Rectangles());
}

HRESULT SwapChainD3D11::VerifyReadback(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const D3D11_TEXTURE2D_DESC& sourceDesc) noexcept
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
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT result = device->CreateTexture2D(&desc, nullptr, readbackTexture_.put());
        if (FAILED(result))
        {
            return result;
        }
    }
    context->CopyResource(readbackTexture_.get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT result = context->Map(
        readbackTexture_.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        return result;
    }
    auto unmap = wil::scope_exit([&]() noexcept {
        context->Unmap(readbackTexture_.get(), 0);
    });
    const size_t rowBytes = static_cast<size_t>(sourceDesc.Width) *
        (sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U);
    if (mapped.RowPitch < rowBytes || output_->ScanoutPitch() < rowBytes)
    {
        return E_UNEXPECTED;
    }
    bool matched = true;
    for (NvU32 rowIndex = 0; rowIndex < sourceDesc.Height; ++rowIndex)
    {
        auto* destination = static_cast<NvU8*>(output_->BackPixels()) +
            static_cast<size_t>(output_->ScanoutPitch()) * rowIndex;
        const auto* reference = static_cast<const NvU8*>(mapped.pData) +
            static_cast<size_t>(mapped.RowPitch) * rowIndex;
        if (std::memcmp(destination, reference, rowBytes) != 0)
        {
            matched = false;
        }
        if (!matched)
        {
            std::memcpy(destination, reference, rowBytes);
        }
    }
    TraceEvents(
        matched ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
        TRACE_DRIVER,
        "ReadFromSubresource verification: matched=%u bytes=%llu pitch=%u pointerMod128=%u",
        matched, static_cast<NvU64>(rowBytes) * sourceDesc.Height,
        output_->ScanoutPitch(),
        static_cast<NvU32>(reinterpret_cast<ULONG_PTR>(output_->BackPixels()) & 127U));
    if (!matched)
    {
        readFromSubresourceSupported_ = false;
        copyTiming_.path = 2;
        const HRESULT damageResult = damage_.Invalidate(output_->BackBufferIndex());
        if (FAILED(damageResult))
        {
            return damageResult;
        }
    }
    readbackVerified_ = matched;
    return S_OK;
}

HRESULT SwapChainD3D11::CopyFrame(
    wil::com_ptr_nothrow<IDXGIResource>& surface) noexcept
{
    if (output_->BackPixels() == nullptr || output_->ScanoutPitch() == 0)
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
         sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         sourceDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
    {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "Unsupported swap-chain surface: %ux%u format=%u expected=%ux%u",
            sourceDesc.Width,
            sourceDesc.Height,
            sourceDesc.Format,
            mode->hVisible,
            mode->vVisible);
        return DXGI_ERROR_UNSUPPORTED;
    }

    copyTiming_.width = sourceDesc.Width;
    copyTiming_.height = sourceDesc.Height;
    copyTiming_.format = sourceDesc.Format;
    if (damage_.Rectangles().empty())
    {
        return S_OK;
    }

    wil::com_ptr_nothrow<ID3D11Device> device;
    source->GetDevice(device.put());
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    const bool tryReadFromSubresource = readbackMode_ == 1 ||
        (readbackMode_ == 2 && ((textureFrameCount_ / 120) & 1U) != 0);
    copyTiming_.readFromSubresourceRequested = tryReadFromSubresource;
    ++textureFrameCount_;
    const bool matchingFormat =
        (output_->ScanoutFormat() == NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16 &&
         sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (output_->ScanoutFormat() == NV_KMS_WINDOWS_SCANOUT_FORMAT_XRGB8 &&
         (sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
          sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
          sourceDesc.Format == DXGI_FORMAT_B8G8R8X8_UNORM));
    if (textureFrameCount_ == 1)
    {
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "Readback surface: width=%u height=%u format=%u usage=%u cpu=0x%08X bind=0x%08X misc=0x%08X pitch=%u pointerMod128=%u",
            sourceDesc.Width, sourceDesc.Height, sourceDesc.Format,
            sourceDesc.Usage, sourceDesc.CPUAccessFlags, sourceDesc.BindFlags,
            sourceDesc.MiscFlags, output_->ScanoutPitch(),
            static_cast<NvU32>(reinterpret_cast<ULONG_PTR>(output_->BackPixels()) & 127U));
    }
    if (tryReadFromSubresource && readFromSubresourceSupported_ && matchingFormat &&
        sourceDesc.Usage == D3D11_USAGE_DEFAULT && sourceDesc.SampleDesc.Count == 1 &&
        sourceDesc.MipLevels == 1 && sourceDesc.ArraySize == 1)
    {
        wil::com_ptr_nothrow<ID3D11Device3> device3;
        result = device.query_to(device3.put());
        if (SUCCEEDED(result))
        {
            const bool verifyReadback = !readbackVerified_ ||
                ((readbackMode_ == 2 || damage_.Enabled()) && (textureFrameCount_ % 120) == 1);
            ID3D11Texture2D* readSource = source.get();
            NvU32 readPath = 3;
            result = DXGI_ERROR_UNSUPPORTED;
            if ((sourceDesc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0)
            {
                const NvU64 mapStart = PerformanceCounter();
                result = context->Map(readSource, 0, D3D11_MAP_READ, 0, nullptr);
                copyTiming_.mapTicks += PerformanceCounter() - mapStart;
            }
            if (FAILED(result) && !defaultReadbackRejected_)
            {
                bool replace = !defaultReadbackTexture_;
                if (!replace)
                {
                    D3D11_TEXTURE2D_DESC desc = {};
                    defaultReadbackTexture_->GetDesc(&desc);
                    replace = desc.Width != sourceDesc.Width ||
                        desc.Height != sourceDesc.Height || desc.Format != sourceDesc.Format;
                }
                if (replace)
                {
                    D3D11_TEXTURE2D_DESC1 desc = {};
                    desc.Width = sourceDesc.Width;
                    desc.Height = sourceDesc.Height;
                    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.Format = sourceDesc.Format;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
                    wil::com_ptr_nothrow<ID3D11Texture2D1> texture;
                    const NvU64 stagingStart = PerformanceCounter();
                    result = device3->CreateTexture2D1(&desc, nullptr, texture.put());
                    if (SUCCEEDED(result))
                    {
                        result = texture.query_to(defaultReadbackTexture_.put());
                    }
                    copyTiming_.stagingTicks += PerformanceCounter() - stagingStart;
                    if (FAILED(result))
                    {
                        defaultReadbackRejected_ = true;
                    }
                }
                if (defaultReadbackTexture_ && !defaultReadbackRejected_)
                {
                    const NvU64 readbackStart = PerformanceCounter();
                    if (damage_.Enabled() && !verifyReadback)
                    {
                        for (const RECT& rectangle : damage_.Rectangles())
                        {
                            const D3D11_BOX box = {
                                static_cast<UINT>(rectangle.left), static_cast<UINT>(rectangle.top), 0,
                                static_cast<UINT>(rectangle.right), static_cast<UINT>(rectangle.bottom), 1};
                            context->CopySubresourceRegion(defaultReadbackTexture_.get(), 0,
                                box.left, box.top, 0, source.get(), 0, &box);
                        }
                    }
                    else
                    {
                        context->CopyResource(defaultReadbackTexture_.get(), source.get());
                    }
                    context->Flush();
                    copyTiming_.readbackTicks += PerformanceCounter() - readbackStart;
                    source.reset();
                    surface.reset();
                    result = FinishFrame();
                    if (FAILED(result))
                    {
                        return result;
                    }
                    source = defaultReadbackTexture_;
                    readSource = source.get();
                    readPath = 4;
                    const NvU64 mapStart = PerformanceCounter();
                    result = context->Map(readSource, 0, D3D11_MAP_READ, 0, nullptr);
                    copyTiming_.mapTicks += PerformanceCounter() - mapStart;
                    if (FAILED(result))
                    {
                        defaultReadbackRejected_ = true;
                    }
                }
            }
            if (SUCCEEDED(result))
            {
                const NvU64 readStart = PerformanceCounter();
                if (damage_.Enabled())
                {
                    for (const RECT& rectangle : damage_.Rectangles())
                    {
                        const D3D11_BOX box = {
                            static_cast<UINT>(rectangle.left), static_cast<UINT>(rectangle.top), 0,
                            static_cast<UINT>(rectangle.right), static_cast<UINT>(rectangle.bottom), 1};
                        auto* pixels = static_cast<NvU8*>(output_->BackPixels()) +
                            static_cast<size_t>(rectangle.top) * output_->ScanoutPitch() +
                            static_cast<size_t>(rectangle.left) *
                                (sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8U : 4U);
                        device3->ReadFromSubresource(pixels, output_->ScanoutPitch(), 0,
                            readSource, 0, &box);
                    }
                }
                else
                {
                    device3->ReadFromSubresource(
                        output_->BackPixels(), output_->ScanoutPitch(), 0, readSource, 0, nullptr);
                }
                copyTiming_.copyTicks += PerformanceCounter() - readStart;
                const NvU64 unmapStart = PerformanceCounter();
                context->Unmap(readSource, 0);
                copyTiming_.unmapTicks += PerformanceCounter() - unmapStart;
                copyTiming_.path = readPath;
                if (verifyReadback)
                {
                    copyTiming_.verification = true;
                    result = VerifyReadback(device.get(), context.get(), readSource, sourceDesc);
                    if (FAILED(result))
                    {
                        return result;
                    }
                }
                return device->GetDeviceRemovedReason();
            }
            if (textureFrameCount_ <= 8 || (textureFrameCount_ % 60) == 0)
            {
                TraceEvents(
                    TRACE_LEVEL_INFORMATION,
                    TRACE_DRIVER,
                    "ReadFromSubresource unavailable: result=0x%08X format=%u sourceCpu=0x%08X",
                    result, sourceDesc.Format, sourceDesc.CPUAccessFlags);
            }
        }
    }
    ID3D11Texture2D* mappedTexture = source.get();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    NvU64 mapStart = PerformanceCounter();
    result = context->Map(mappedTexture, 0, D3D11_MAP_READ, 0, &mapped);
    copyTiming_.mapTicks += PerformanceCounter() - mapStart;
    copyTiming_.path = 1;
    if (FAILED(result))
    {
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_DRIVER,
            "Direct Map failed: result=0x%08X format=%u usage=%u bind=0x%08X cpu=0x%08X misc=0x%08X",
            result,
            sourceDesc.Format,
            sourceDesc.Usage,
            sourceDesc.BindFlags,
            sourceDesc.CPUAccessFlags,
            sourceDesc.MiscFlags);
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
            const NvU64 stagingStart = PerformanceCounter();
            result = device->CreateTexture2D(&desc, nullptr, readbackTexture_.put());
            copyTiming_.stagingTicks += PerformanceCounter() - stagingStart;
            if (FAILED(result))
            {
                TraceEvents(
                    TRACE_LEVEL_ERROR,
                    TRACE_DRIVER,
                    "Create staging texture failed: result=0x%08X format=%u",
                    result,
                    desc.Format);
                return result;
            }
        }
        mappedTexture = readbackTexture_.get();
        copyTiming_.path = 2;
        const NvU64 readbackStart = PerformanceCounter();
        if (damage_.Enabled())
        {
            for (const RECT& rectangle : damage_.Rectangles())
            {
                const D3D11_BOX box = {
                    static_cast<UINT>(rectangle.left), static_cast<UINT>(rectangle.top), 0,
                    static_cast<UINT>(rectangle.right), static_cast<UINT>(rectangle.bottom), 1};
                context->CopySubresourceRegion(mappedTexture, 0, box.left, box.top, 0,
                    source.get(), 0, &box);
            }
        }
        else
        {
            context->CopyResource(mappedTexture, source.get());
        }
        context->Flush();
        copyTiming_.readbackTicks += PerformanceCounter() - readbackStart;
        source.reset();
        surface.reset();
        result = FinishFrame();
        if (FAILED(result))
        {
            return result;
        }
        mapStart = PerformanceCounter();
        result = context->Map(mappedTexture, 0, D3D11_MAP_READ, 0, &mapped);
        copyTiming_.mapTicks += PerformanceCounter() - mapStart;
        if (FAILED(result))
        {
            TraceEvents(
                TRACE_LEVEL_ERROR,
                TRACE_DRIVER,
                "Staging Map failed: result=0x%08X format=%u",
                result,
                sourceDesc.Format);
            return result;
        }
    }

    // Mapping must be undone on every pitch/format/copy exit.
    auto unmap = wil::scope_exit([&]() noexcept {
        const NvU64 unmapStart = PerformanceCounter();
        context->Unmap(mappedTexture, 0);
        copyTiming_.unmapTicks += PerformanceCounter() - unmapStart;
    });
    IDDCX_SYSTEM_BUFFER_INFO buffer = {};
    buffer.Width = sourceDesc.Width;
    buffer.Height = sourceDesc.Height;
    buffer.Format = sourceDesc.Format;
    buffer.Pitch = mapped.RowPitch;
    buffer.pBuffer = mapped.pData;
    return CopyFrame(buffer);
}

_Use_decl_annotations_
void SwapChainD3D11::Cleanup(WDFOBJECT object)
{
    GetSwapChainD3D11(object)->~SwapChainD3D11();
}

} // namespace nv::iddcx
