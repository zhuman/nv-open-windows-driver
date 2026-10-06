#include "swapchain_pipeline.hpp"
#include "output.hpp"
#include "display_modes.hpp"
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
    if (enabled_)
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
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (!pending)
    {
        NvU64 timestamp = 0;
        if (NvKmsWindowsGetFlipTimestamp(output_->AssignedHead(), &timestamp) != NV_OK)
            return E_FAIL;
        auto guard = lock_.lock_exclusive();
        const HRESULT result = slots_.Latch(latest);
        if (FAILED(result))
            return result;
        output_->SetFrontBuffer(latest);
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
                &job.layer, 1, NV_FALSE, &submitTiming);
            if (status == NV_OK)
            {
                submitted_ = true;
                lastSubmissionMs_ = GetTickCount64();
                output_->SetSubmittedBuffer(destination);
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

DWORD WINAPI SwapChainUploadPipeline::ThreadEntry(void* context) noexcept
{
    SwapChainThreadScheduling scheduling("CE");
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
                result = Consume(job);
        }
        else
        {
            result = Refresh();
            bool pending;
            {
                auto guard = lock_.lock_exclusive();
                pending = slots_.Latest() != FrameSlots::Invalid;
            }
            const HANDLE waits[] = {stopEvent_, wake_.get()};
            if (WaitForMultipleObjects(2, waits, FALSE, pending ? 1 : INFINITE) == WAIT_FAILED)
                result = HRESULT_FROM_WIN32(GetLastError());
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
    return result_.load();
}

} // namespace nv::iddcx
