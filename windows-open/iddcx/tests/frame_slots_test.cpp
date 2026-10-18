#include <gtest/gtest.h>

#include "..\frame_slots.hpp"
#include "..\dirty_regions.hpp"
#include <algorithm>
#include <vector>
#include <random>

using nv::iddcx::FrameSlots;
using nv::iddcx::FrameRamSlots;


// Deferred uploads must reproduce the producer's captured snapshot, not a
// newer frame that arrived while the upload or flip was waiting.
void DeferredDamage(UINT count)
{
    constexpr UINT Width = 16, Height = 12;
    FrameSlots slots;
    nv::iddcx::DirtyRegionTracker damage;
    ASSERT_TRUE(SUCCEEDED(slots.Configure(count, 0)));
    ASSERT_TRUE(SUCCEEDED(damage.Configure(Width, Height, count)));
    std::vector<unsigned> incoming(Width * Height);
    std::vector<std::vector<unsigned>> ram(count, std::vector<unsigned>(Width * Height, ~0U));
    auto vram = ram;
    auto snapshots = ram;
    nv::iddcx::DirtyRegions jobs[4];
    std::mt19937 random(count * 619);
    unsigned frame = 0;
    for (UINT step = 0; step < 100000; ++step)
    {
        const UINT index = random() % count;
        switch (slots.Get(index))
        {
        case FrameSlots::State::Free:
        {
            const UINT acquired = slots.Acquire();
            ASSERT_NE(acquired, FrameSlots::Invalid);
            ++frame;
            const LONG x = random() % Width, y = random() % Height;
            const RECT rectangle = {x, y, x + 1, y + 1};
            incoming[static_cast<UINT>(y) * Width + static_cast<UINT>(x)] = frame;
            EXPECT_TRUE(SUCCEEDED(damage.AddFrame({&rectangle, 1})));
            EXPECT_TRUE(SUCCEEDED(damage.Pending(acquired, &jobs[acquired])));
            for (const RECT& region : jobs[acquired].Rectangles())
            {
                const UINT first = static_cast<UINT>(region.top) * Width;
                const UINT last = static_cast<UINT>(region.bottom) * Width;
                std::copy(incoming.begin() + first, incoming.begin() + last, ram[acquired].begin() + first);
            }
            snapshots[acquired] = incoming;
            EXPECT_TRUE(ram[acquired] == incoming);
            // Only the producer consumes damage, before ingesting a later frame.
            EXPECT_TRUE(SUCCEEDED(damage.Commit(acquired)));
            EXPECT_TRUE(SUCCEEDED(slots.Ready(acquired)));
            break;
        }
        case FrameSlots::State::Ready:
            EXPECT_TRUE(SUCCEEDED(slots.Upload(index)));
            for (const RECT& region : jobs[index].Rectangles())
            {
                const UINT first = static_cast<UINT>(region.top) * Width;
                const UINT last = static_cast<UINT>(region.bottom) * Width;
                std::copy(ram[index].begin() + first, ram[index].begin() + last, vram[index].begin() + first);
            }
            EXPECT_TRUE(ram[index] == snapshots[index]);
            EXPECT_TRUE(vram[index] == snapshots[index]);
            break;
        case FrameSlots::State::Uploading:
            if (slots.Queued() < 2)
                EXPECT_TRUE(SUCCEEDED(slots.Queue(index)));
            break;
        case FrameSlots::State::Queued:
            if (index == slots.Latest())
            {
                EXPECT_TRUE(vram[index] == snapshots[index]);
                EXPECT_TRUE(SUCCEEDED(slots.Latch(index)));
            }
            break;
        default:
            break;
        }
    }
}

// RAM and scanout indices are separate ownership domains. Coalescing CPU
// snapshots must preserve damage owed to each independently reused VRAM slot.
void IndependentDamage(UINT count)
{
    constexpr UINT Width = 16, Height = 12;
    FrameSlots display;
    FrameRamSlots memory;
    nv::iddcx::DirtyRegionTracker ramDamage, vramDamage;
    ASSERT_TRUE(SUCCEEDED(display.Configure(count, 0)));
    ASSERT_TRUE(SUCCEEDED(memory.Configure(count)));
    ASSERT_TRUE(SUCCEEDED(ramDamage.Configure(Width, Height, count)));
    ASSERT_TRUE(SUCCEEDED(vramDamage.Configure(Width, Height, count)));
    std::vector<unsigned> incoming(Width * Height);
    std::vector<std::vector<unsigned>> ram(count, std::vector<unsigned>(Width * Height, ~0U));
    auto vram = ram, snapshots = ram;
    nv::iddcx::DirtyRegions jobs[4];
    std::vector<UINT> queue;
    std::mt19937 random(count * 157);
    unsigned frame = 0, sharedIndexCopies = 0, coalesced = 0;
    const auto copy = [](const auto& source, auto& target, const nv::iddcx::DirtyRegions& regions) {
        for (const RECT& rectangle : regions.Rectangles())
        {
            const UINT first = static_cast<UINT>(rectangle.top) * Width;
            const UINT last = static_cast<UINT>(rectangle.bottom) * Width;
            std::copy(source.begin() + first, source.begin() + last, target.begin() + first);
        }
    };
    for (UINT step = 0; step < 100000; ++step)
    {
        const UINT action = random() % 3;
        if (action == 0)
        {
            const UINT slot = memory.Acquire();
            if (slot == FrameSlots::Invalid)
                continue;
            sharedIndexCopies += display.Get(slot) == FrameSlots::State::Front;
            ++frame;
            const UINT x = random() % Width, y = random() % Height;
            const RECT rectangle = {static_cast<LONG>(x), static_cast<LONG>(y),
                static_cast<LONG>(x + 1), static_cast<LONG>(y + 1)};
            incoming[y * Width + x] = frame;
            EXPECT_TRUE(SUCCEEDED(ramDamage.AddFrame({&rectangle, 1})));
            EXPECT_TRUE(SUCCEEDED(ramDamage.Pending(slot, &jobs[slot])));
            copy(incoming, ram[slot], jobs[slot]);
            EXPECT_TRUE(ram[slot] == incoming);
            snapshots[slot] = incoming;
            EXPECT_TRUE(SUCCEEDED(ramDamage.Commit(slot)));
            EXPECT_TRUE(SUCCEEDED(memory.Ready(slot)));
            queue.push_back(slot);
        }
        else if (action == 1 && !queue.empty() && display.Queued() < 2)
        {
            const UINT destination = display.Acquire();
            if (destination == FrameSlots::Invalid)
                continue;
            // Collapse only complete, unconsumed snapshots; preserve their VRAM damage.
            if (random() % 2)
                while (queue.size() > 1)
                {
                    const UINT dropped = queue.front();
                    EXPECT_TRUE(SUCCEEDED(vramDamage.AddFrame(jobs[dropped].Rectangles())));
                    EXPECT_TRUE(SUCCEEDED(memory.Read(dropped)));
                    EXPECT_TRUE(SUCCEEDED(memory.Release(dropped)));
                    queue.erase(queue.begin());
                    ++coalesced;
                }
            const UINT source = queue.front();
            queue.erase(queue.begin());
            EXPECT_TRUE(SUCCEEDED(memory.Read(source)));
            EXPECT_TRUE(SUCCEEDED(vramDamage.AddFrame(jobs[source].Rectangles())));
            nv::iddcx::DirtyRegions regions;
            EXPECT_TRUE(SUCCEEDED(vramDamage.Pending(destination, &regions)));
            EXPECT_TRUE(SUCCEEDED(display.Ready(destination)));
            EXPECT_TRUE(SUCCEEDED(display.Upload(destination)));
            copy(ram[source], vram[destination], regions);
            EXPECT_TRUE(vram[destination] == snapshots[source]);
            EXPECT_TRUE(SUCCEEDED(vramDamage.Commit(destination)));
            EXPECT_TRUE(SUCCEEDED(memory.Release(source)));
            EXPECT_TRUE(SUCCEEDED(display.Queue(destination)));
        }
        else if (action == 2 && display.Latest() != FrameSlots::Invalid)
            EXPECT_TRUE(SUCCEEDED(display.Latch(display.Latest())));
    }
    EXPECT_TRUE(sharedIndexCopies > 100 && coalesced > 100);
}

TEST(FrameSlots, EnforcesLeasesQueueDepthAndStaleNotifierOwnership)
{
    FrameRamSlots memory;
    EXPECT_TRUE(SUCCEEDED(memory.Configure(2)));
    EXPECT_TRUE(memory.Acquire() == 0 && memory.Acquire() == 1);
    EXPECT_TRUE(SUCCEEDED(memory.Ready(0)) && SUCCEEDED(memory.Read(0)));
    EXPECT_TRUE(memory.Acquire() == FrameSlots::Invalid);
    EXPECT_TRUE(FAILED(memory.Release(1)));
    EXPECT_TRUE(SUCCEEDED(memory.Release(0)) && memory.Acquire() == 0);
    FrameSlots pool;
    EXPECT_TRUE(FAILED(pool.Configure(1, 0)));
    EXPECT_TRUE(FAILED(pool.Configure(5, 0)));
    EXPECT_TRUE(FAILED(pool.Configure(3, 3)));
    EXPECT_TRUE(SUCCEEDED(pool.Configure(4, 0)));
    const UINT first = pool.Acquire();
    const UINT second = pool.Acquire();
    const UINT third = pool.Acquire();
    EXPECT_TRUE(first == 1 && second == 2 && third == 3);
    EXPECT_TRUE(pool.Acquire() == FrameSlots::Invalid);
    EXPECT_TRUE(FAILED(pool.Queue(first)));
    EXPECT_TRUE(SUCCEEDED(pool.Ready(first)));
    EXPECT_TRUE(FAILED(pool.Ready(first)));
    EXPECT_TRUE(SUCCEEDED(pool.Upload(first)));
    EXPECT_TRUE(SUCCEEDED(pool.Queue(first)));
    EXPECT_TRUE(SUCCEEDED(pool.Ready(second)));
    EXPECT_TRUE(SUCCEEDED(pool.Upload(second)));
    EXPECT_TRUE(SUCCEEDED(pool.Queue(second)));
    EXPECT_TRUE(SUCCEEDED(pool.Ready(third)));
    EXPECT_TRUE(SUCCEEDED(pool.Upload(third)));
    EXPECT_TRUE(FAILED(pool.Queue(third)));
    EXPECT_TRUE(FAILED(pool.Latch(first))); // An old notifier cannot retire the newer update.
    EXPECT_TRUE(pool.Get(0) == FrameSlots::State::Front);
    EXPECT_TRUE(pool.Get(first) == FrameSlots::State::Queued);
    EXPECT_TRUE(SUCCEEDED(pool.Latch(second)));
    EXPECT_TRUE(pool.Get(second) == FrameSlots::State::Front);
    EXPECT_TRUE(pool.Get(third) == FrameSlots::State::Uploading);
    EXPECT_TRUE(SUCCEEDED(pool.Queue(third)));
    EXPECT_TRUE(SUCCEEDED(pool.Latch(third)));
    EXPECT_TRUE(FAILED(pool.Latch(third)));

}

TEST(FrameSlots, ModelsDeferredDamageAcrossTwoThreeAndFourBuffers)
{
    for (UINT count = 2; count <= 4; ++count)
    {
        SCOPED_TRACE(count);
        DeferredDamage(count);
    }
}

TEST(FrameSlots, ModelsIndependentRamVramDamageAndCoalescing)
{
    for (UINT count = 2; count <= 4; ++count)
    {
        SCOPED_TRACE(count);
        IndependentDamage(count);
    }
}

TEST(FrameSlots, ModelsOwnershipAcrossTwoThreeAndFourBuffers)
{
    // Random interleavings must never lend the current front to the producer.
    std::mt19937 random(871);
    for (UINT count = 2; count <= 4; ++count)
    {
        SCOPED_TRACE(count);
        FrameSlots slots;
        ASSERT_TRUE(SUCCEEDED(slots.Configure(count, count - 1)));
        UINT front = count - 1;
        for (UINT step = 0; step < 100000; ++step)
        {
            const UINT index = random() % count;
            switch (slots.Get(index))
            {
            case FrameSlots::State::Free:
            {
                const UINT acquired = slots.Acquire();
                EXPECT_TRUE(acquired != FrameSlots::Invalid && acquired != front);
                break;
            }
            case FrameSlots::State::Copying:
                EXPECT_TRUE(SUCCEEDED(slots.Ready(index)));
                break;
            case FrameSlots::State::Ready:
                EXPECT_TRUE(SUCCEEDED(slots.Upload(index)));
                break;
            case FrameSlots::State::Uploading:
                if (slots.Queued() < 2)
                    EXPECT_TRUE(SUCCEEDED(slots.Queue(index)));
                break;
            case FrameSlots::State::Queued:
                if (index == slots.Latest())
                {
                    EXPECT_TRUE(SUCCEEDED(slots.Latch(index)));
                    front = index;
                }
                else
                    EXPECT_TRUE(FAILED(slots.Latch(index)));
                break;
            case FrameSlots::State::Front:
                break;
            }
            UINT fronts = 0;
            for (UINT slot = 0; slot < count; ++slot)
                fronts += slots.Get(slot) == FrameSlots::State::Front;
            EXPECT_TRUE(fronts == 1 && slots.Get(front) == FrameSlots::State::Front);
            EXPECT_TRUE(slots.Queued() <= 2);
        }
    }
}
