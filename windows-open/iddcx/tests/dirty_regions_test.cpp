#include <gtest/gtest.h>

#include "..\dirty_regions.hpp"

#include <algorithm>
#include <vector>

using nv::iddcx::DirtyRegions;
using nv::iddcx::DirtyRegionTracker;

namespace {


DirtyRegions Pending(DirtyRegionTracker& tracker, UINT slot)
{
    DirtyRegions regions;
    if (FAILED(tracker.Pending(slot, &regions)))
    {
        ADD_FAILURE() << "Unable to query pending dirty regions for slot " << slot;
        return {};
    }
    if (regions.count > 2)
    {
        ADD_FAILURE() << "Unexpected dirty-region count: " << regions.count;
        return {};
    }
    for (UINT index = 0; index < regions.count; ++index)
    {
        const RECT& r = regions.rectangles[index];
        EXPECT_TRUE(r.left == 0 && r.right == static_cast<LONG>(tracker.Width()));
        EXPECT_TRUE(r.top >= 0 && r.top < r.bottom &&
            r.bottom <= static_cast<LONG>(tracker.Height()));
        EXPECT_TRUE(index == 0 || regions.rectangles[index - 1].bottom < r.top);
    }
    return regions;
}

TEST(DirtyRegions, ValidatesBoundsCoalescingAndResize)
{
    DirtyRegionTracker tracker;
    DirtyRegions regions;
    EXPECT_TRUE(FAILED(tracker.Pending(0, &regions)));
    EXPECT_TRUE(FAILED(tracker.Configure(0, 100, 2)));
    EXPECT_TRUE(FAILED(tracker.Configure(100, 100, 0)));
    EXPECT_TRUE(FAILED(tracker.Configure(0xffffffffU, 100, 2)));
    ASSERT_TRUE(SUCCEEDED(tracker.Configure(100, 100, 3)));
    for (UINT slot = 0; slot < 3; ++slot)
    {
        const auto initial = Pending(tracker, slot);
        EXPECT_TRUE(initial.count == 1 && initial.rectangles[0].bottom == 100);
        EXPECT_TRUE(SUCCEEDED(tracker.Commit(slot)));
    }
    EXPECT_TRUE(FAILED(tracker.Commit(3)));
    EXPECT_TRUE(FAILED(tracker.Pending(0, nullptr)));
    const RECT rectangles[] = {{4, 10, 9, 20}, {4, 30, 9, 40}, {4, 80, 9, 90}};
    // The bounded band list may over-copy clean rows, but must not drop damage.
    EXPECT_TRUE(SUCCEEDED(tracker.AddFrame(rectangles)));
    const auto collapsed = Pending(tracker, 0);
    EXPECT_TRUE(collapsed.count == 2);
    EXPECT_TRUE(collapsed.rectangles[0].top == 10 && collapsed.rectangles[0].bottom == 40);
    EXPECT_TRUE(collapsed.rectangles[1].top == 80 && collapsed.rectangles[1].bottom == 90);
    EXPECT_TRUE(SUCCEEDED(tracker.Commit(0)));
    EXPECT_TRUE(Pending(tracker, 0).count == 0);
    EXPECT_TRUE(Pending(tracker, 1).count == 2);
    const RECT empty[] = {{0, 0, 0, 0}, {-20, 4, -10, 8}, {101, 4, 110, 8}};
    EXPECT_TRUE(SUCCEEDED(tracker.AddFrame(empty)));
    EXPECT_TRUE(Pending(tracker, 0).count == 0);
    const RECT clipped[] = {{-4, -5, 8, 5}, {99, 95, 105, 105}};
    EXPECT_TRUE(SUCCEEDED(tracker.AddFrame(clipped)));
    EXPECT_TRUE(Pending(tracker, 0).rectangles[0].top == 0);
    EXPECT_TRUE(Pending(tracker, 0).rectangles[1].bottom == 100);
    const RECT invalid[] = {{10, 10, 5, 20}};
    EXPECT_TRUE(FAILED(tracker.AddFrame(invalid)));
    tracker.Invalidate();
    for (UINT slot = 0; slot < 3; ++slot)
    {
        EXPECT_TRUE(Pending(tracker, slot).rectangles[0].bottom == 100);
    }
    EXPECT_TRUE(SUCCEEDED(tracker.Configure(7, 5, 1)));
    EXPECT_TRUE(Pending(tracker, 0).rectangles[0].right == 7);
    EXPECT_TRUE(Pending(tracker, 0).rectangles[0].bottom == 5);
}

unsigned Random(unsigned& state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

// The full incoming image is the oracle; seeded slot reuse checks cumulative
// damage independently for every RAM/VRAM copy, including dropped uploads.
void ModelFrames(UINT slots)
{
    constexpr UINT Width = 32;
    constexpr UINT Height = 24;
    DirtyRegionTracker tracker;
    ASSERT_TRUE(SUCCEEDED(tracker.Configure(Width, Height, slots)));
    std::vector<unsigned> incoming(Width * Height, 0);
    std::vector<std::vector<unsigned>> ram(slots, std::vector<unsigned>(Width * Height, ~0U));
    std::vector<std::vector<unsigned>> vram = ram;
    unsigned random = slots;
    for (unsigned frame = 1; frame <= 20000; ++frame)
    {
        std::vector<RECT> rectangles;
        const unsigned updates = Random(random) % 5;
        for (unsigned index = 0; index < updates; ++index)
        {
            LONG x = static_cast<LONG>(Random(random) % (Width + 10)) - 5;
            LONG y = static_cast<LONG>(Random(random) % (Height + 10)) - 5;
            RECT r = {x, y, x + 1 + static_cast<LONG>(Random(random) % 8),
                y + 1 + static_cast<LONG>(Random(random) % 6)};
            rectangles.push_back(r);
            for (LONG row = (std::max)(0L, r.top); row < (std::min)(static_cast<LONG>(Height), r.bottom); ++row)
            {
                for (LONG col = (std::max)(0L, r.left); col < (std::min)(static_cast<LONG>(Width), r.right); ++col)
                {
                    incoming[static_cast<UINT>(row) * Width + static_cast<UINT>(col)] = frame;
                }
            }
        }
        EXPECT_TRUE(SUCCEEDED(tracker.AddFrame(rectangles)));
        const UINT slot = Random(random) % slots;
        const auto pending = Pending(tracker, slot);
        for (const RECT& r : pending.Rectangles())
        {
            const UINT first = static_cast<UINT>(r.top) * Width;
            const UINT last = static_cast<UINT>(r.bottom) * Width;
            std::copy(incoming.begin() + first, incoming.begin() + last, ram[slot].begin() + first);
        }
        // A dropped flip updates RAM but must not consume the VRAM slot's damage.
        if (Random(random) % 3 == 0)
        {
            continue;
        }
        for (const RECT& r : pending.Rectangles())
        {
            const UINT first = static_cast<UINT>(r.top) * Width;
            const UINT last = static_cast<UINT>(r.bottom) * Width;
            std::copy(ram[slot].begin() + first, ram[slot].begin() + last, vram[slot].begin() + first);
        }
        EXPECT_TRUE(vram[slot] == incoming);
        EXPECT_TRUE(SUCCEEDED(tracker.Commit(slot)));
        EXPECT_TRUE(Pending(tracker, slot).count == 0);
    }
}

} // namespace

TEST(DirtyRegions, ModelsOneBuffer) { ModelFrames(1); }
TEST(DirtyRegions, ModelsTwoBuffers) { ModelFrames(2); }
TEST(DirtyRegions, ModelsThreeBuffers) { ModelFrames(3); }
TEST(DirtyRegions, ModelsSevenBuffers) { ModelFrames(7); }
TEST(DirtyRegions, ModelsSeventeenBuffers) { ModelFrames(17); }
