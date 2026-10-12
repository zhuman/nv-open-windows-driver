#include "swapchain_pipeline.hpp"
#include "output.hpp"
#include "display_modes.hpp"
#include "mpo_scene.hpp"
#include "swapchain_priority.hpp"
#include "trace.h"

#include "swapchain_pipeline.tmh"

namespace nv::iddcx {
namespace {
NvU64 Counter() noexcept
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<NvU64>(value.QuadPart);
}
}

SwapChainUploadPipeline::~SwapChainUploadPipeline() noexcept
{
    Stop();
}

HRESULT SwapChainUploadPipeline::Start(Output& output, HANDLE stopEvent, NvU32 sourceApi) noexcept
{
    output_ = &output;
    stopEvent_ = stopEvent;
    sourceApi_ = sourceApi;
    scene_ = output.DemoScene();
    if (scene_) scene_->Restart();
    enabled_ = output.PresentPolicy().pipeline != 0;
    if (output.RequiresScanoutReset())
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Pipeline requires a new modeset after failed CE/flip; refusing to reuse uncertain scanout storage");
        return HRESULT_FROM_WIN32(ERROR_DEVICE_HARDWARE_ERROR);
    }
    const ULONGLONG deadline = GetTickCount64() + 2000;
    NvBool pending = NV_TRUE;
    while (pending)
    {
        const NV_STATUS status = NvKmsWindowsGetFlipPending(output.AssignedHead(), &pending);
        if (status != NV_OK || (pending && GetTickCount64() > deadline))
        {
            const HRESULT result = status == NV_OK ?
                HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Pipeline startup retirement failed: status=0x%08X result=0x%08X", status, result);
            return result;
        }
        if (pending)
            Sleep(1);
    }
    output.SetFrontBuffer(output.SubmittedBufferIndex());
    const HRESULT configured = slots_.Configure(
        output.PresentPolicy().buffers, output.FrontBufferIndex());
    if (FAILED(configured))
        return configured;
    HRESULT result = ramSlots_.Configure(output.PresentPolicy().buffers);
    if (FAILED(result))
        return result;
    const auto* mode = display_modes::FindDisplayMode(output.DisplayState(), output.ActiveModeIndex());
    if (mode == nullptr)
        return E_UNEXPECTED;
    width_ = mode->hVisible;
    result = vramDamage_.Configure(width_, mode->vVisible, output.PresentPolicy().buffers);
    if (FAILED(result))
        return result;
    wake_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    available_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!wake_ || !available_)
        return HRESULT_FROM_WIN32(GetLastError());
    if (scene_)
    {
        animationTimer_.reset(CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE));
        if (!animationTimer_)
        {
            const DWORD error = GetLastError();
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "MPO high-resolution timer creation failed: %u", error);
            return HRESULT_FROM_WIN32(error);
        }
    }
    if (enabled_ || scene_)
    {
        thread_.reset(CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr));
        if (!thread_)
            return HRESULT_FROM_WIN32(GetLastError());
    }
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return E_FAIL;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Copy pipeline: api=%u enabled=%u buffers=%u flip=%u vrr=%u frequency=%llu",
        sourceApi_, enabled_, output.PresentPolicy().buffers, output.PresentPolicy().flip,
        output.VrrEnabled(), static_cast<NvU64>(frequency.QuadPart));
    return S_OK;
}

void SwapChainUploadPipeline::Fail(HRESULT result) noexcept
{
    result_.store(result);
    Parameters().Reject("Presentation failed; inspect the driver trace before retrying");
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Copy pipeline failed: result=0x%08X", result);
    SetEvent(stopEvent_);
    SetEvent(available_.get());
    SetEvent(wake_.get());
}

HRESULT SwapChainUploadPipeline::Refresh() noexcept
{
    UINT latest;
    {
        auto guard = lock_.lock_exclusive();
        latest = slots_.Latest();
    }
    if (latest == FrameSlots::Invalid)
        return S_OK;
    NvBool pending = NV_FALSE;
    const NV_STATUS status = NvKmsWindowsGetFlipPending(output_->AssignedHead(), &pending);
    if (status != NV_OK)
        return E_FAIL;
    if (pending && GetTickCount64() - lastSubmissionMs_ > 2000)
    {
        TraceLayerRetirement();
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (!pending)
    {
        if (scene_)
            for (ULONG index = 1; index <= scene_->PlaneCount(); ++index)
            {
                NvBool overlayPending = NV_FALSE;
                if (NvKmsWindowsGetLayerFlipPending(
                    output_->AssignedHead(), index, &overlayPending) != NV_OK) return E_FAIL;
                if (overlayPending)
                {
                    if (GetTickCount64() - lastSubmissionMs_ > 2000)
                    {
                        TraceLayerRetirement();
                        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                    }
                    return S_OK;
                }
            }
        NvU64 timestamp = 0;
        if (NvKmsWindowsGetFlipTimestamp(output_->AssignedHead(), &timestamp) != NV_OK)
            return E_FAIL;
        auto guard = lock_.lock_exclusive();
        const HRESULT result = slots_.Latch(latest);
        if (FAILED(result))
            return result;
        output_->SetFrontBuffer(latest);
        output_->RetirePreviousPool();
        output_->PresentationApplied();
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "Pipeline latch observed: slot=%u frame=%llu qpc=%llu timestamp=%llu",
            latest, queuedFrames_[latest], Counter(), timestamp);
        SetEvent(available_.get());
    }
    return S_OK;
}

HRESULT SwapChainUploadPipeline::Acquire(UINT* index) noexcept
{
    for (;;)
    {
        if (FAILED(result_.load()))
            return result_.load();
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        {
            auto guard = lock_.lock_exclusive();
            *index = ramSlots_.Acquire();
            if (*index != FrameSlots::Invalid)
            {
                output_->SelectCopyBuffer(*index);
                return S_OK;
            }
        }
        if (!enabled_)
        {
            const HRESULT result = Refresh();
            if (FAILED(result))
                return result;
        }
        SetEvent(wake_.get());
        const HANDLE waits[] = {stopEvent_, available_.get()};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 2);
        if (wait == WAIT_FAILED)
            return HRESULT_FROM_WIN32(GetLastError());
    }
}

HRESULT SwapChainUploadPipeline::Submit(UINT index, NvU64 frame,
    const NV_KMS_WINDOWS_LAYER& layer, ID3D12Resource* mapped,
    NvU64 sourceStart, NvU64 sourceEnd, NvU64 gpuSubmit, NvU64 gpuComplete) noexcept
{
    auto guard = lock_.lock_exclusive();
    const HRESULT ready = ramSlots_.Ready(index);
    if (FAILED(ready))
        return ready;
    auto& job = jobs_[index];
    job.slot = index;
    job.frame = frame;
    job.layer = layer;
    if (layer.colorTransform != nullptr)
        job.transform = *layer.colorTransform;
    job.layer.colorTransform = &job.transform;
    job.mapped = mapped;
    job.sourceStart = sourceStart;
    job.sourceEnd = sourceEnd;
    job.gpuSubmit = gpuSubmit;
    job.gpuComplete = gpuComplete;
    if (enabled_)
    {
        queue_[queueCount_++] = index;
        SetEvent(wake_.get());
        return S_OK;
    }
    Job current = job;
    current.layer.colorTransform = &current.transform;
    job.mapped = nullptr;
    guard.reset();
    return Consume(current);
}

void SwapChainUploadPipeline::Unmap(Job& job) noexcept
{
    if (job.mapped != nullptr)
    {
        const D3D12_RANGE written = {0, 0};
        job.mapped->Unmap(0, &written);
        job.mapped = nullptr;
    }
}

HRESULT SwapChainUploadPipeline::AddDamage(const Job& job) noexcept
{
    if (!job.layer.useDirtyBands)
    {
        vramDamage_.Invalidate();
        return S_OK;
    }
    RECT rectangles[2] = {};
    if (job.layer.dirtyBandCount > ARRAYSIZE(rectangles))
        return E_INVALIDARG;
    for (UINT index = 0; index < job.layer.dirtyBandCount; ++index)
        rectangles[index] = {0, static_cast<LONG>(job.layer.dirtyBands[index].top),
            static_cast<LONG>(width_), static_cast<LONG>(job.layer.dirtyBands[index].bottom)};
    return vramDamage_.AddFrame({rectangles, job.layer.dirtyBandCount});
}

HRESULT SwapChainUploadPipeline::Consume(Job& job) noexcept
{
    wil::rwlock_release_exclusive_scope_exit presentation;
    if (scene_) presentation = presentLock_.lock_exclusive();
    auto unmap = wil::scope_exit([&]() noexcept { Unmap(job); });
    HRESULT result = AddDamage(job);
    if (FAILED(result))
        return result;
    {
        auto guard = lock_.lock_exclusive();
        result = ramSlots_.Read(job.slot);
        if (FAILED(result))
            return result;
    }
    UINT destination = FrameSlots::Invalid;
    const ULONGLONG leaseDeadline = GetTickCount64() + 2000;
    while (destination == FrameSlots::Invalid)
    {
        result = Refresh();
        if (FAILED(result))
            return result;
        {
            auto guard = lock_.lock_exclusive();
            destination = slots_.Acquire();
        }
        if (destination == FrameSlots::Invalid)
        {
            if (WaitForSingleObject(stopEvent_, 1) == WAIT_OBJECT_0)
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if (GetTickCount64() > leaseDeadline)
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    DirtyRegions regions;
    result = vramDamage_.Pending(destination, &regions);
    if (FAILED(result))
        return result;
    job.layer.uploadSource = job.layer.surface;
    job.layer.surface = output_->Surface(destination);
    if (job.layer.useDirtyBands)
    {
        job.layer.dirtyBandCount = regions.count;
        for (UINT index = 0; index < regions.count; ++index)
        {
            job.layer.dirtyBands[index].top = regions.rectangles[index].top;
            job.layer.dirtyBands[index].bottom = regions.rectangles[index].bottom;
        }
    }
    {
        auto guard = lock_.lock_exclusive();
        result = slots_.Ready(destination);
        if (SUCCEEDED(result))
            result = slots_.Upload(destination);
        if (FAILED(result))
            return result;
    }
    NV_KMS_WINDOWS_PRESENT_TIMING timing = {};
    const NV_STATUS upload = NvKmsWindowsUploadLayer(&job.layer, &timing);
    Unmap(job);
    if (upload != NV_OK)
    {
        output_->RequireScanoutReset();
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "Pipeline CE failed: status=0x%08X slot=%u frame=%llu", upload, job.slot, job.frame);
        return E_FAIL;
    }
    result = vramDamage_.Commit(destination);
    if (FAILED(result))
        return result;
    {
        auto guard = lock_.lock_exclusive();
        result = ramSlots_.Release(job.slot);
        if (FAILED(result))
            return result;
        SetEvent(available_.get());
    }
    job.layer.skipUpload = NV_TRUE;
    NV_KMS_WINDOWS_LAYER layers[8] = {};
    NvU32 layerCount = 1;
    layers[0] = job.layer;
    if (scene_)
    {
        result = WaitForLayers();
        if (FAILED(result)) return result;
        result = scene_->Prepare(job.layer, layers, &layerCount);
        if (FAILED(result)) return result;
    }
    const ULONGLONG deadline = GetTickCount64() + 2000;
    for (;;)
    {
        const HRESULT refreshed = Refresh();
        if (FAILED(refreshed))
            return refreshed;
        UINT queued;
        {
            auto guard = lock_.lock_exclusive();
            queued = slots_.Queued();
        }
        NvBool pending = NV_FALSE;
        if (!submitted_ || output_->PresentPolicy().flip == 0)
        {
            if (NvKmsWindowsGetFlipPending(output_->AssignedHead(), &pending) != NV_OK)
                return E_FAIL;
        }
        if (!pending && queued < (output_->PresentPolicy().flip ? 2U : 1U))
        {
            NV_KMS_WINDOWS_PRESENT_TIMING submitTiming = {};
            const NV_STATUS status = NvKmsWindowsPresentLayersTimed(
                output_->AssignedHead(), output_->DisplayState().displayHandle,
                layers, layerCount, NV_FALSE, &submitTiming);
            if (status == NV_OK)
            {
                submitted_ = true;
                lastSubmissionMs_ = GetTickCount64();
                output_->SetSubmittedBuffer(destination);
                if (scene_)
                {
                    const HRESULT planeQueued = scene_->Submitted();
                    if (FAILED(planeQueued)) return planeQueued;
                    demoSubmitted_ = true;
                    desktop_ = job.layer;
                    desktop_.uploadSource = nullptr;
                    desktop_.reuseSurface = NV_TRUE;
                    desktopTransform_ = job.transform;
                    desktop_.colorTransform = &desktopTransform_;
                }
                auto guard = lock_.lock_exclusive();
                result = slots_.Queue(destination);
                if (FAILED(result))
                    return result;
                queuedFrames_[destination] = job.frame;
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                    "Pipeline frame: frame=%llu slot=%u bytes=%llu regions=%u uploadStart=%llu uploadEnd=%llu queuedPrior=%u vrrFlip=%u",
                    job.frame, destination, timing.uploadBytes, timing.uploadRegions,
                    timing.uploadStart, timing.uploadEnd, queued, submitTiming.vrrFlip);
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                    "Pipeline source: frame=%llu slot=%u ram=%u api=%u sourceStart=%llu sourceEnd=%llu submit=%llu gpuSubmit=%llu gpuComplete=%llu",
                    job.frame, destination, job.slot, sourceApi_, job.sourceStart, job.sourceEnd, Counter(),
                    job.gpuSubmit, job.gpuComplete);
                return S_OK;
            }
            if (status != NV_ERR_IN_USE)
            {
                output_->RequireScanoutReset();
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Pipeline flip failed: status=0x%08X slot=%u frame=%llu", status, job.slot, job.frame);
                return E_FAIL;
            }
        }
        if (WaitForSingleObject(stopEvent_, 1) == WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (GetTickCount64() > deadline)
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
}

void SwapChainUploadPipeline::TraceLayerRetirement() noexcept
{
    const NV_STATUS channelStatus = NvKmsWindowsTraceFlipChannels(output_->AssignedHead());
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
        "MPO retirement channel query: head=%u status=0x%08X",
        output_->AssignedHead(), channelStatus);
    const NvU32 planeCount = scene_ ? scene_->PlaneCount() : 0;
    for (NvU32 index = 0; index <= planeCount; ++index)
    {
        NvBool busy = NV_FALSE;
        NvU64 timestamp = 0;
        const NV_STATUS pendingStatus = NvKmsWindowsGetLayerFlipPending(
            output_->AssignedHead(), index, &busy);
        const NV_STATUS timestampStatus = NvKmsWindowsGetLayerFlipTimestamp(
            output_->AssignedHead(), index, &timestamp);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "MPO retirement timeout: head=%u layer=%u pending=%u timestamp=%llu query=0x%08X timeQuery=0x%08X",
            output_->AssignedHead(), index, busy, timestamp, pendingStatus, timestampStatus);
    }
}

HRESULT SwapChainUploadPipeline::WaitForLayers() noexcept
{
    const auto deadline = GetTickCount64() + 2000;
    for (;;)
    {
        bool overlayPending = false;
        HRESULT result = scene_->Retire(&overlayPending);
        if (FAILED(result)) return result;
        NvBool primaryPending = NV_FALSE;
        if (NvKmsWindowsGetFlipPending(output_->AssignedHead(), &primaryPending) != NV_OK)
            return E_FAIL;
        result = Refresh();
        if (FAILED(result)) return result;
        if (!primaryPending && !overlayPending) return S_OK;
        if (WaitForSingleObject(stopEvent_, 1) == WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (GetTickCount64() > deadline)
        {
            TraceLayerRetirement();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
}

HRESULT SwapChainUploadPipeline::Animate() noexcept
{
    auto state = output_->LockFrameStateShared();
    auto presentation = presentLock_.lock_exclusive();
    if (!demoSubmitted_ || !scene_->Due()) return S_OK;
    const HRESULT retired = WaitForLayers();
    if (FAILED(retired)) return retired;
    NV_KMS_WINDOWS_LAYER layers[8] = {};
    NvU32 count = 0;
    desktop_.surface = output_->ScanoutSurface();
    desktopTransform_ = output_->ColorTransform();
    const HRESULT prepared = scene_->Prepare(desktop_, layers, &count);
    if (FAILED(prepared)) return prepared;
    const NV_STATUS presented = NvKmsWindowsPresentLayersTimed(output_->AssignedHead(),
        output_->DisplayState().displayHandle, layers, count, NV_FALSE, nullptr);
    if (presented != NV_OK)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "MPO overlay-only flip failed: status=0x%08X", presented);
        return E_FAIL;
    }
    const HRESULT queued = scene_->Submitted();
    if (FAILED(queued)) return queued;
    lastSubmissionMs_ = GetTickCount64();
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DRIVER,
        "MPO overlay-only frame: connector=%u mode=%u count=%u",
        output_->DisplayState().connectorIndex, output_->Config().demo, count);
    return S_OK;
}

HRESULT SwapChainUploadPipeline::DisableDemo() noexcept
{
    if (!scene_ || !demoSubmitted_) return S_OK;
    // Source and animation threads are stopped; cancellation cannot free a live plane.
    const auto deadline = GetTickCount64() + 2000;
    for (unsigned phase = 0; phase < 2; ++phase)
    {
        for (;;)
        {
            bool overlays = false;
            NvBool primary = NV_FALSE;
            const HRESULT retired = scene_->Retire(&overlays);
            if (FAILED(retired) ||
                NvKmsWindowsGetFlipPending(output_->AssignedHead(), &primary) != NV_OK)
                return E_FAIL;
            if (!overlays && !primary) break;
            if (GetTickCount64() > deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            Sleep(1);
        }
        if (phase == 0)
        {
            const HRESULT refreshed = Refresh();
            if (FAILED(refreshed)) return refreshed;
            desktop_.surface = output_->ScanoutSurface();
            if (NvKmsWindowsPresentLayersTimed(output_->AssignedHead(),
                output_->DisplayState().displayHandle, &desktop_, 1, NV_FALSE, nullptr) != NV_OK)
                return E_FAIL;
        }
    }
    demoSubmitted_ = false;
    return S_OK;
}

DWORD WINAPI SwapChainUploadPipeline::ThreadEntry(void* context) noexcept
{
    auto* pipeline = static_cast<SwapChainUploadPipeline*>(context);
    SwapChainThreadScheduling scheduling("CE", pipeline->output_->Config().mmcss != 0);
    static_cast<SwapChainUploadPipeline*>(context)->Process();
    return 0;
}

void SwapChainUploadPipeline::Process() noexcept
{
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0)
    {
        UINT index = FrameSlots::Invalid;
        Job job;
        auto unmap = wil::scope_exit([&]() noexcept { Unmap(job); });
        UINT skipped[4] = {};
        UINT skippedCount = 0;
        {
            auto guard = lock_.lock_exclusive();
            if (queueCount_ != 0)
            {
                if (output_->PresentPolicy().flip != 0)
                {
                    skippedCount = queueCount_ - 1;
                    for (UINT skip = 0; skip < skippedCount; ++skip)
                        skipped[skip] = queue_[skip];
                }
                index = queue_[skippedCount];
                job = jobs_[index];
                job.layer.colorTransform = &job.transform;
                jobs_[index].mapped = nullptr;
                const UINT consumed = skippedCount + 1;
                for (UINT next = consumed; next < queueCount_; ++next)
                    queue_[next - consumed] = queue_[next];
                queueCount_ -= consumed;
            }
        }
        HRESULT result = S_OK;
        for (UINT skip = 0; skip < skippedCount && SUCCEEDED(result); ++skip)
        {
            auto& previous = jobs_[skipped[skip]];
            result = AddDamage(previous);
            Unmap(previous);
            const NvU64 frame = previous.frame;
            {
                auto guard = lock_.lock_exclusive();
                if (SUCCEEDED(result))
                    result = ramSlots_.Read(skipped[skip]);
                if (SUCCEEDED(result))
                    result = ramSlots_.Release(skipped[skip]);
            }
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                "Pipeline coalesced ready frame: frame=%llu ram=%u", frame, skipped[skip]);
            SetEvent(available_.get());
        }
        if (index != FrameSlots::Invalid)
        {
            if (SUCCEEDED(result))
            {
                auto state = output_->LockFrameStateShared();
                result = Consume(job);
            }
        }
        else
        {
            result = Refresh();
            if (SUCCEEDED(result) && scene_) result = Animate();
            bool pending;
            {
                auto guard = lock_.lock_exclusive();
                pending = slots_.Latest() != FrameSlots::Invalid;
            }
            if (SUCCEEDED(result) && scene_)
            {
                LARGE_INTEGER due = {};
                due.QuadPart = -static_cast<LONGLONG>(scene_->Wait100ns());
                if (!SetWaitableTimer(animationTimer_.get(), &due, 0, nullptr, nullptr, FALSE))
                    result = HRESULT_FROM_WIN32(GetLastError());
                else
                {
                    const HANDLE waits[] = {stopEvent_, wake_.get(), animationTimer_.get()};
                    if (WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE,
                        pending ? 1 : INFINITE) == WAIT_FAILED)
                        result = HRESULT_FROM_WIN32(GetLastError());
                }
            }
            else if (SUCCEEDED(result))
            {
                const HANDLE waits[] = {stopEvent_, wake_.get()};
                if (WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE,
                    pending ? 1 : INFINITE) == WAIT_FAILED)
                    result = HRESULT_FROM_WIN32(GetLastError());
            }
        }
        if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            Fail(result);
            break;
        }
    }
}

HRESULT SwapChainUploadPipeline::Stop() noexcept
{
    if (output_ == nullptr)
        return result_.load();
    SetEvent(stopEvent_);
    if (thread_)
    {
        SetEvent(wake_.get());
        WaitForSingleObject(thread_.get(), INFINITE);
        thread_.reset();
    }
    for (auto& job : jobs_)
        if (job.mapped != nullptr)
        {
            const D3D12_RANGE written = {0, 0};
            job.mapped->Unmap(0, &written);
            job.mapped = nullptr;
        }
    const HRESULT disabled = DisableDemo();
    if (FAILED(disabled))
    {
        output_->RequireScanoutReset();
        Fail(disabled);
    }
    if (output_ != nullptr && submitted_)
    {
        const NvU64 deadline = GetTickCount64() + 2000;
        NvBool pending = NV_TRUE;
        while (pending)
        {
            if (NvKmsWindowsGetFlipPending(output_->AssignedHead(), &pending) != NV_OK)
            {
                Fail(E_FAIL);
                break;
            }
            if (pending && GetTickCount64() > deadline)
            {
                Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                break;
            }
            if (pending)
                Sleep(1);
        }
        if (!pending)
        {
            const HRESULT refreshed = Refresh();
            if (FAILED(refreshed))
                Fail(refreshed);
        }
    }
    output_ = nullptr;
    animationTimer_.reset();
    return result_.load();
}

} // namespace nv::iddcx
