#include "..\dirty_regions.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using nv::iddcx::DirtyRegions;
using nv::iddcx::DirtyRegionTracker;

namespace {

void Require(bool value)
{
    if (!value)
    {
        std::fputs("Dirty-region test failed\n", stderr);
        std::abort();
    }
}

DirtyRegions Pending(DirtyRegionTracker& tracker, UINT slot)
{
    DirtyRegions regions;
    Require(SUCCEEDED(tracker.Pending(slot, &regions)));
    Require(regions.count <= 2);
    for (UINT index = 0; index < regions.count; ++index)
    {
        const RECT& r = regions.rectangles[index];
        Require(r.left == 0 && r.right == static_cast<LONG>(tracker.Width()));
        Require(r.top >= 0 && r.top < r.bottom &&
            r.bottom <= static_cast<LONG>(tracker.Height()));
        Require(index == 0 || regions.rectangles[index - 1].bottom < r.top);
    }
    return regions;
}

void BasicCases()
{
    DirtyRegionTracker tracker;
    DirtyRegions regions;
    Require(FAILED(tracker.Pending(0, &regions)));
    Require(FAILED(tracker.Configure(0, 100, 2)));
    Require(FAILED(tracker.Configure(100, 100, 0)));
    Require(FAILED(tracker.Configure(0xffffffffU, 100, 2)));
    Require(SUCCEEDED(tracker.Configure(100, 100, 3)));
    for (UINT slot = 0; slot < 3; ++slot)
    {
        const auto initial = Pending(tracker, slot);
        Require(initial.count == 1 && initial.rectangles[0].bottom == 100);
        Require(SUCCEEDED(tracker.Commit(slot)));
    }
    Require(FAILED(tracker.Commit(3)));
    Require(FAILED(tracker.Pending(0, nullptr)));
    const RECT rectangles[] = {{4, 10, 9, 20}, {4, 30, 9, 40}, {4, 80, 9, 90}};
    Require(SUCCEEDED(tracker.AddFrame(rectangles)));
    const auto collapsed = Pending(tracker, 0);
    Require(collapsed.count == 2);
    Require(collapsed.rectangles[0].top == 10 && collapsed.rectangles[0].bottom == 40);
    Require(collapsed.rectangles[1].top == 80 && collapsed.rectangles[1].bottom == 90);
    Require(SUCCEEDED(tracker.Commit(0)));
    Require(Pending(tracker, 0).count == 0);
    Require(Pending(tracker, 1).count == 2);
    const RECT empty[] = {{0, 0, 0, 0}, {-20, 4, -10, 8}, {101, 4, 110, 8}};
    Require(SUCCEEDED(tracker.AddFrame(empty)));
    Require(Pending(tracker, 0).count == 0);
    const RECT clipped[] = {{-4, -5, 8, 5}, {99, 95, 105, 105}};
    Require(SUCCEEDED(tracker.AddFrame(clipped)));
    Require(Pending(tracker, 0).rectangles[0].top == 0);
    Require(Pending(tracker, 0).rectangles[1].bottom == 100);
    const RECT invalid[] = {{10, 10, 5, 20}};
    Require(FAILED(tracker.AddFrame(invalid)));
    tracker.Invalidate();
    for (UINT slot = 0; slot < 3; ++slot)
    {
        Require(Pending(tracker, slot).rectangles[0].bottom == 100);
    }
    Require(SUCCEEDED(tracker.Configure(7, 5, 1)));
    Require(Pending(tracker, 0).rectangles[0].right == 7);
    Require(Pending(tracker, 0).rectangles[0].bottom == 5);
}

unsigned Random(unsigned& state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

void ModelFrames(UINT slots)
{
    constexpr UINT Width = 32;
    constexpr UINT Height = 24;
    DirtyRegionTracker tracker;
    Require(SUCCEEDED(tracker.Configure(Width, Height, slots)));
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
        Require(SUCCEEDED(tracker.AddFrame(rectangles)));
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
        Require(vram[slot] == incoming);
        Require(SUCCEEDED(tracker.Commit(slot)));
        Require(Pending(tracker, slot).count == 0);
    }
}

} // namespace

int main()
{
    BasicCases();
    for (UINT slots : {1U, 2U, 3U, 7U, 17U})
    {
        ModelFrames(slots);
    }
    std::puts("PASS: boundaries, coalescing, resize, and 100000 modeled frames across 1/2/3/7/17 buffers");
}
