#pragma once

#include "dirty_regions.hpp"
#include <wdf.h>
#include <iddcx.h>
#include "nvkms_windows.h"

namespace nv::iddcx {

class SwapChainDamage final
{
public:
    HRESULT Initialize(IDDCX_SWAPCHAIN handle, UINT bufferCount) noexcept;
    HRESULT Prepare(UINT width, UINT height, UINT frameNumber, UINT dirtyCount,
        UINT moveCount, UINT bufferIndex) noexcept;
    HRESULT Commit(UINT bufferIndex) noexcept;
    HRESULT Invalidate(UINT bufferIndex) noexcept;
    void ApplyToLayer(NV_KMS_WINDOWS_LAYER& layer) const noexcept;
    bool Enabled() const noexcept { return enabled_; }
    std::span<const RECT> Rectangles() const noexcept { return regions_.Rectangles(); }

private:
    IDDCX_SWAPCHAIN handle_ = nullptr;
    UINT bufferCount_ = 0;
    bool enabled_ = false;
    bool haveFrame_ = false;
    UINT frameNumber_ = 0;
    std::unique_ptr<RECT[]> scratch_;
    UINT scratchCount_ = 0;
    DirtyRegionTracker tracker_;
    DirtyRegions regions_;
};

} // namespace nv::iddcx
