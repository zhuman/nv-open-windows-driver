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

NvU16 Unorm8ToFp16(NvU8 value) noexcept
{
    if (value == 0)
    {
        return 0;
    }
    const float normalized = static_cast<float>(value) / 255.0F;
    NvU32 bits = 0;
    std::memcpy(&bits, &normalized, sizeof(bits));
    NvU32 exponent = (bits >> 23) & 0xffU;
    NvU32 mantissa = bits & 0x7fffffU;
    mantissa += 0xfffU + ((mantissa >> 13) & 1U);
    if ((mantissa & 0x800000U) != 0)
    {
        mantissa = 0;
        ++exponent;
    }
    return static_cast<NvU16>(((exponent - 112U) << 10) | (mantissa >> 13));
}

void CopyHdrMetadata(
    const IDDCX_HDR10_METADATA& source,
    NV_KMS_WINDOWS_HDR_METADATA* destination) noexcept
{
    const USHORT* primaries[] = {
        source.RedPrimary, source.GreenPrimary, source.BluePrimary };
    for (NvU32 primary = 0; primary < 3; ++primary)
    {
        destination->displayPrimaries[primary][0] = primaries[primary][0];
        destination->displayPrimaries[primary][1] = primaries[primary][1];
    }
    destination->whitePoint[0] = source.WhitePoint[0];
    destination->whitePoint[1] = source.WhitePoint[1];
    destination->maxMasteringLuminance = source.MaxMasteringLuminance;
    destination->minMasteringLuminance = source.MinMasteringLuminance;
    destination->maxContentLightLevel = source.MaxContentLightLevel;
    destination->maxFrameAverageLightLevel = source.MaxFrameAverageLightLevel;
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
        const NvU64 frameStart = PerformanceCounter();
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        IDARG_IN_RELEASEANDACQUIREBUFFER2 input2 = {};
        IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer2 = {};
        if (useBuffer2_)
        {
            input2.Size = sizeof(input2);
            input2.AcquireSystemMemoryBuffer = useSystemMemory_;
            buffer2.MetaData.Size = sizeof(buffer2.MetaData);
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
            result = hasSystemBuffer
                ? CopyFrame(buffer2.MetaData.SystemBufferInfo)
                : CopyFrame(acquiredSurface);
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
        TraceEvents(frameCount < 8 || frameCount % 60 == 0 ?
            TRACE_LEVEL_INFORMATION : TRACE_LEVEL_VERBOSE,
            TRACE_DRIVER,
            "Frame timing us: frame=%u acquire=%llu stateWait=%llu staging=%llu readback=%llu map=%llu cpuCopy=%llu unmap=%llu kmsWait=%llu upload=%llu prepare=%llu submit=%llu finish=%llu total=%llu idle=%llu dropped=%u pending=%llu buffer=%u",
            frameCount, Microseconds(acquireTicks), Microseconds(stateWaitTicks),
            Microseconds(copyTiming_.stagingTicks), Microseconds(copyTiming_.readbackTicks),
            Microseconds(copyTiming_.mapTicks), Microseconds(copyTiming_.copyTicks),
            Microseconds(copyTiming_.unmapTicks), Microseconds(presentTiming.lockTicks),
            Microseconds(presentTiming.uploadTicks), Microseconds(presentTiming.prepareTicks),
            Microseconds(presentTiming.submitTicks), Microseconds(copyTiming_.finishTicks),
            Microseconds(PerformanceCounter() - frameStart), Microseconds(idleTicks), dropped,
            Microseconds(presentTiming.pendingTicks), bufferIndex);
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
        CopyHdrMetadata(
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
    const NvU64 copyStart = PerformanceCounter();
    auto recordCopy = wil::scope_exit([&]() noexcept {
        copyTiming_.copyTicks += PerformanceCounter() - copyStart;
    });
    if (output_->BackPixels() == nullptr || output_->ScanoutPitch() == 0 ||
        buffer.pBuffer == nullptr)
    {
        return E_UNEXPECTED;
    }
    const auto* mode = display_modes::FindDisplayMode(
        output_->DisplayState(), output_->ActiveModeIndex());
    if (mode == nullptr || buffer.Width != mode->hVisible ||
        buffer.Height != mode->vVisible)
    {
        return E_UNEXPECTED;
    }

    auto* destination = static_cast<NvU8*>(output_->BackPixels());
    const auto* source = static_cast<const NvU8*>(buffer.pBuffer);
    if (output_->ScanoutFormat() == NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16)
    {
        const size_t rowBytes = static_cast<size_t>(mode->hVisible) * 8U;
        if (output_->ScanoutPitch() < rowBytes)
        {
            return E_UNEXPECTED;
        }
        if (buffer.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            for (NvU32 y = 0; y < mode->vVisible; ++y)
            {
                std::memcpy(
                    destination + output_->ScanoutPitch() * y,
                    source + buffer.Pitch * y,
                    rowBytes);
            }
            return S_OK;
        }
        if (buffer.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            buffer.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
            buffer.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            buffer.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            return DXGI_ERROR_UNSUPPORTED;
        }
        const bool rgba = buffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            buffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        for (NvU32 y = 0; y < mode->vVisible; ++y)
        {
            auto* destinationRow = reinterpret_cast<NvU16*>(
                destination + output_->ScanoutPitch() * y);
            const NvU8* sourceRow = source + buffer.Pitch * y;
            for (NvU32 x = 0; x < mode->hVisible; ++x)
            {
                destinationRow[x * 4] = Unorm8ToFp16(
                    sourceRow[x * 4 + (rgba ? 0 : 2)]);
                destinationRow[x * 4 + 1] = Unorm8ToFp16(sourceRow[x * 4 + 1]);
                destinationRow[x * 4 + 2] = Unorm8ToFp16(
                    sourceRow[x * 4 + (rgba ? 2 : 0)]);
                destinationRow[x * 4 + 3] = Unorm8ToFp16(sourceRow[x * 4 + 3]);
            }
        }
        return S_OK;
    }

    if (buffer.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        buffer.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        buffer.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
        buffer.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        buffer.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    const size_t rowBytes = static_cast<size_t>(mode->hVisible) * 4U;
    if (output_->ScanoutPitch() < rowBytes)
    {
        return E_UNEXPECTED;
    }
    const bool rgba = buffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        buffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (NvU32 y = 0; y < mode->vVisible; ++y)
    {
        NvU8* destinationRow = destination + output_->ScanoutPitch() * y;
        const NvU8* sourceRow = source + buffer.Pitch * y;
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

    wil::com_ptr_nothrow<ID3D11Device> device;
    source->GetDevice(device.put());
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    ID3D11Texture2D* mappedTexture = source.get();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    NvU64 mapStart = PerformanceCounter();
    result = context->Map(mappedTexture, 0, D3D11_MAP_READ, 0, &mapped);
    copyTiming_.mapTicks += PerformanceCounter() - mapStart;
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
        const NvU64 readbackStart = PerformanceCounter();
        context->CopyResource(mappedTexture, source.get());
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
