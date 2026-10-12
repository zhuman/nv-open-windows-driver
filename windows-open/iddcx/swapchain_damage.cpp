#include "swapchain_damage.hpp"
#include "driver_parameters.hpp"
#include "trace.h"

#include <new>

#include "swapchain_damage.tmh"

namespace nv::iddcx {

HRESULT SwapChainDamage::Initialize(IDDCX_SWAPCHAIN handle, UINT bufferCount, bool enabled) noexcept
{
    if (enabled != 0 && !IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainGetDirtyRects))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "IddCx dirty-rectangle API unavailable");
        return E_NOTIMPL;
    }
    handle_ = handle;
    bufferCount_ = bufferCount;
    enabled_ = enabled;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Swapchain damage: enabled=%u buffers=%u maximumBands=2", enabled_, bufferCount_);
    return S_OK;
}

HRESULT SwapChainDamage::Prepare(UINT width, UINT height, UINT frameNumber,
    UINT dirtyCount, UINT moveCount, UINT bufferIndex) noexcept
{
    if (tracker_.Width() != width || tracker_.Height() != height)
    {
        const HRESULT result = tracker_.Configure(width, height, bufferCount_);
        if (FAILED(result))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Dirty-region initialization failed: result=0x%08X", result);
            return result;
        }
        haveFrame_ = false;
    }
    if (!enabled_)
    {
        tracker_.Invalidate();
        return tracker_.Pending(bufferIndex, &regions_);
    }
    const bool discontinuity = haveFrame_ && frameNumber != frameNumber_ &&
        frameNumber != frameNumber_ + 1U;
    if (moveCount != 0 || discontinuity || dirtyCount > 65536 ||
        (dirtyCount == 0 && (!haveFrame_ || frameNumber != frameNumber_)))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "Dirty metadata requires full refresh: previous=%u current=%u dirty=%u moves=%u gap=%u",
            frameNumber_, frameNumber, dirtyCount, moveCount, discontinuity);
        tracker_.Invalidate();
    }
    else if (dirtyCount != 0)
    {
        if (scratchCount_ < dirtyCount)
        {
            std::unique_ptr<RECT[]> scratch(new (std::nothrow) RECT[dirtyCount]);
            if (!scratch)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Dirty-rectangle allocation failed");
                return E_OUTOFMEMORY;
            }
            scratch_ = std::move(scratch);
            scratchCount_ = dirtyCount;
        }
        IDARG_IN_GETDIRTYRECTS input = {};
        input.DirtyRectInCount = dirtyCount;
        input.pDirtyRects = scratch_.get();
        IDARG_OUT_GETDIRTYRECTS output = {};
        HRESULT result = IddCxSwapChainGetDirtyRects(handle_, &input, &output);
        if (SUCCEEDED(result) && output.DirtyRectOutCount != dirtyCount)
        {
            result = E_UNEXPECTED;
        }
        if (SUCCEEDED(result))
        {
            result = tracker_.AddFrame({scratch_.get(), dirtyCount});
        }
        if (FAILED(result))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "Dirty metadata retrieval/validation failed: result=0x%08X count=%u returned=%u",
                result, dirtyCount, output.DirtyRectOutCount);
            return result;
        }
    }
    frameNumber_ = frameNumber;
    haveFrame_ = true;
    return tracker_.Pending(bufferIndex, &regions_);
}

HRESULT SwapChainDamage::Commit(UINT bufferIndex) noexcept
{
    return tracker_.Commit(bufferIndex);
}

HRESULT SwapChainDamage::Invalidate(UINT bufferIndex) noexcept
{
    tracker_.Invalidate();
    return tracker_.Pending(bufferIndex, &regions_);
}

void SwapChainDamage::ApplyToLayer(NV_KMS_WINDOWS_LAYER& layer) const noexcept
{
    layer.useDirtyBands = enabled_;
    layer.dirtyBandCount = regions_.count;
    for (UINT index = 0; index < regions_.count; ++index)
    {
        layer.dirtyBands[index].top = static_cast<NvU32>(regions_.rectangles[index].top);
        layer.dirtyBands[index].bottom = static_cast<NvU32>(regions_.rectangles[index].bottom);
    }
}

} // namespace nv::iddcx
