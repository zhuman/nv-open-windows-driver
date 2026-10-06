#pragma once

#include <windows.h>
#include <array>
#include <memory>
#include <span>

namespace nv::iddcx {

struct DirtyRegions final
{
    std::array<RECT, 2> rectangles = {};
    UINT count = 0;
    std::span<const RECT> Rectangles() const noexcept
    {
        return {rectangles.data(), count};
    }
};

class DirtyRegionTracker final
{
public:
    HRESULT Configure(UINT width, UINT height, UINT bufferCount) noexcept;
    HRESULT AddFrame(std::span<const RECT> rectangles) noexcept;
    HRESULT Pending(UINT bufferIndex, DirtyRegions* regions) const noexcept;
    HRESULT Commit(UINT bufferIndex) noexcept;
    void Invalidate() noexcept;
    UINT Width() const noexcept { return width_; }
    UINT Height() const noexcept { return height_; }

private:
    void AddRectangle(DirtyRegions& regions, const RECT& rectangle) const noexcept;
    std::unique_ptr<DirtyRegions[]> pending_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT bufferCount_ = 0;
};

} // namespace nv::iddcx
