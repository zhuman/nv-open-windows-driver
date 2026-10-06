#include "dirty_regions.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace nv::iddcx {

HRESULT DirtyRegionTracker::Configure(UINT width, UINT height, UINT bufferCount) noexcept
{
    if (width == 0 || height == 0 || bufferCount == 0 ||
        width > static_cast<UINT>((std::numeric_limits<LONG>::max)()) ||
        height > static_cast<UINT>((std::numeric_limits<LONG>::max)()))
    {
        return E_INVALIDARG;
    }
    std::unique_ptr<DirtyRegions[]> pending(new (std::nothrow) DirtyRegions[bufferCount]);
    if (!pending)
    {
        return E_OUTOFMEMORY;
    }
    pending_ = std::move(pending);
    width_ = width;
    height_ = height;
    bufferCount_ = bufferCount;
    Invalidate();
    return S_OK;
}

void DirtyRegionTracker::Invalidate() noexcept
{
    for (UINT index = 0; index < bufferCount_; ++index)
    {
        pending_[index].rectangles[0] = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        pending_[index].count = 1;
    }
}

void DirtyRegionTracker::AddRectangle(DirtyRegions& regions, const RECT& rectangle) const noexcept
{
    // RM's CE interface copies linear ranges. Whole-row bands bound submissions
    // to two, avoiding a synchronous control call for every partial-width row.
    std::array<RECT, 3> bands = {};
    UINT count = regions.count;
    std::copy_n(regions.rectangles.begin(), count, bands.begin());
    bands[count++] = {0, rectangle.top, static_cast<LONG>(width_), rectangle.bottom};
    std::sort(bands.begin(), bands.begin() + count,
        [](const RECT& a, const RECT& b) { return a.top < b.top; });
    UINT merged = 0;
    for (UINT index = 0; index < count; ++index)
    {
        if (merged != 0 && bands[index].top <= bands[merged - 1].bottom)
        {
            bands[merged - 1].bottom = (std::max)(bands[merged - 1].bottom, bands[index].bottom);
        }
        else
        {
            bands[merged++] = bands[index];
        }
    }
    if (merged > 2)
    {
        // Preserve the largest untouched gap; conservatively fill the smaller.
        if (bands[1].top - bands[0].bottom >= bands[2].top - bands[1].bottom)
        {
            bands[1].bottom = bands[2].bottom;
        }
        else
        {
            bands[0].bottom = bands[1].bottom;
            bands[1] = bands[2];
        }
        merged = 2;
    }
    std::copy_n(bands.begin(), merged, regions.rectangles.begin());
    regions.count = merged;
}

HRESULT DirtyRegionTracker::AddFrame(std::span<const RECT> rectangles) noexcept
{
    if (!pending_)
    {
        return E_UNEXPECTED;
    }
    for (const RECT& rectangle : rectangles)
    {
        if (rectangle.right < rectangle.left || rectangle.bottom < rectangle.top)
        {
            return E_INVALIDARG;
        }
    }
    for (const RECT& rectangle : rectangles)
    {
        const RECT clipped = {
            (std::max)(0L, rectangle.left), (std::max)(0L, rectangle.top),
            (std::min)(static_cast<LONG>(width_), rectangle.right),
            (std::min)(static_cast<LONG>(height_), rectangle.bottom)};
        if (clipped.left >= clipped.right || clipped.top >= clipped.bottom)
        {
            continue;
        }
        for (UINT index = 0; index < bufferCount_; ++index)
        {
            AddRectangle(pending_[index], clipped);
        }
    }
    return S_OK;
}

HRESULT DirtyRegionTracker::Pending(UINT bufferIndex, DirtyRegions* regions) const noexcept
{
    if (!pending_ || regions == nullptr || bufferIndex >= bufferCount_)
    {
        return E_INVALIDARG;
    }
    *regions = pending_[bufferIndex];
    return S_OK;
}

HRESULT DirtyRegionTracker::Commit(UINT bufferIndex) noexcept
{
    if (!pending_ || bufferIndex >= bufferCount_)
    {
        return E_INVALIDARG;
    }
    pending_[bufferIndex] = {};
    return S_OK;
}

} // namespace nv::iddcx
