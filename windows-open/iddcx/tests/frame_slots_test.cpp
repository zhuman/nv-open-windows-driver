#include "..\frame_slots.hpp"
#include "..\dirty_regions.hpp"
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <random>

using nv::iddcx::FrameSlots;
using nv::iddcx::FrameRamSlots;

void Require(bool value)
{
    if (!value)
    {
        std::fputs("Frame ownership test failed\n", stderr);
        std::abort();
    }
}

void DeferredDamage(UINT count)
{
    constexpr UINT Width = 16, Height = 12;
    FrameSlots slots;
    nv::iddcx::DirtyRegionTracker damage;
    Require(SUCCEEDED(slots.Configure(count, 0)));
    Require(SUCCEEDED(damage.Configure(Width, Height, count)));
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
            Require(acquired != FrameSlots::Invalid);
            ++frame;
            const LONG x = random() % Width, y = random() % Height;
            const RECT rectangle = {x, y, x + 1, y + 1};
            incoming[static_cast<UINT>(y) * Width + static_cast<UINT>(x)] = frame;
            Require(SUCCEEDED(damage.AddFrame({&rectangle, 1})));
            Require(SUCCEEDED(damage.Pending(acquired, &jobs[acquired])));
            for (const RECT& region : jobs[acquired].Rectangles())
            {
                const UINT first = static_cast<UINT>(region.top) * Width;
                const UINT last = static_cast<UINT>(region.bottom) * Width;
                std::copy(incoming.begin() + first, incoming.begin() + last, ram[acquired].begin() + first);
            }
            snapshots[acquired] = incoming;
            Require(ram[acquired] == incoming);
            // Only the producer consumes damage, before ingesting a later frame.
            Require(SUCCEEDED(damage.Commit(acquired)));
            Require(SUCCEEDED(slots.Ready(acquired)));
            break;
        }
        case FrameSlots::State::Ready:
            Require(SUCCEEDED(slots.Upload(index)));
            for (const RECT& region : jobs[index].Rectangles())
            {
                const UINT first = static_cast<UINT>(region.top) * Width;
                const UINT last = static_cast<UINT>(region.bottom) * Width;
                std::copy(ram[index].begin() + first, ram[index].begin() + last, vram[index].begin() + first);
            }
            Require(ram[index] == snapshots[index]);
            Require(vram[index] == snapshots[index]);
            break;
        case FrameSlots::State::Uploading:
            if (slots.Queued() < 2)
                Require(SUCCEEDED(slots.Queue(index)));
            break;
        case FrameSlots::State::Queued:
            if (index == slots.Latest())
            {
                Require(vram[index] == snapshots[index]);
                Require(SUCCEEDED(slots.Latch(index)));
            }
            break;
        default:
            break;
        }
    }
}

void IndependentDamage(UINT count)
{
    constexpr UINT Width = 16, Height = 12;
    FrameSlots display;
    FrameRamSlots memory;
    nv::iddcx::DirtyRegionTracker ramDamage, vramDamage;
    Require(SUCCEEDED(display.Configure(count, 0)));
    Require(SUCCEEDED(memory.Configure(count)));
    Require(SUCCEEDED(ramDamage.Configure(Width, Height, count)));
    Require(SUCCEEDED(vramDamage.Configure(Width, Height, count)));
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
            Require(SUCCEEDED(ramDamage.AddFrame({&rectangle, 1})));
            Require(SUCCEEDED(ramDamage.Pending(slot, &jobs[slot])));
            copy(incoming, ram[slot], jobs[slot]);
            Require(ram[slot] == incoming);
            snapshots[slot] = incoming;
            Require(SUCCEEDED(ramDamage.Commit(slot)));
            Require(SUCCEEDED(memory.Ready(slot)));
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
                    Require(SUCCEEDED(vramDamage.AddFrame(jobs[dropped].Rectangles())));
                    Require(SUCCEEDED(memory.Read(dropped)));
                    Require(SUCCEEDED(memory.Release(dropped)));
                    queue.erase(queue.begin());
                    ++coalesced;
                }
            const UINT source = queue.front();
            queue.erase(queue.begin());
            Require(SUCCEEDED(memory.Read(source)));
            Require(SUCCEEDED(vramDamage.AddFrame(jobs[source].Rectangles())));
            nv::iddcx::DirtyRegions regions;
            Require(SUCCEEDED(vramDamage.Pending(destination, &regions)));
            Require(SUCCEEDED(display.Ready(destination)));
            Require(SUCCEEDED(display.Upload(destination)));
            copy(ram[source], vram[destination], regions);
            Require(vram[destination] == snapshots[source]);
            Require(SUCCEEDED(vramDamage.Commit(destination)));
            Require(SUCCEEDED(memory.Release(source)));
            Require(SUCCEEDED(display.Queue(destination)));
        }
        else if (action == 2 && display.Latest() != FrameSlots::Invalid)
            Require(SUCCEEDED(display.Latch(display.Latest())));
    }
    Require(sharedIndexCopies > 100 && coalesced > 100);
}

int main()
{
    FrameRamSlots memory;
    Require(SUCCEEDED(memory.Configure(2)));
    Require(memory.Acquire() == 0 && memory.Acquire() == 1);
    Require(SUCCEEDED(memory.Ready(0)) && SUCCEEDED(memory.Read(0)));
    Require(memory.Acquire() == FrameSlots::Invalid);
    Require(FAILED(memory.Release(1)));
    Require(SUCCEEDED(memory.Release(0)) && memory.Acquire() == 0);
    FrameSlots pool;
    Require(FAILED(pool.Configure(1, 0)));
    Require(FAILED(pool.Configure(5, 0)));
    Require(FAILED(pool.Configure(3, 3)));
    Require(SUCCEEDED(pool.Configure(4, 0)));
    const UINT first = pool.Acquire();
    const UINT second = pool.Acquire();
    const UINT third = pool.Acquire();
    Require(first == 1 && second == 2 && third == 3);
    Require(pool.Acquire() == FrameSlots::Invalid);
    Require(FAILED(pool.Queue(first)));
    Require(SUCCEEDED(pool.Ready(first)));
    Require(FAILED(pool.Ready(first)));
    Require(SUCCEEDED(pool.Upload(first)));
    Require(SUCCEEDED(pool.Queue(first)));
    Require(SUCCEEDED(pool.Ready(second)));
    Require(SUCCEEDED(pool.Upload(second)));
    Require(SUCCEEDED(pool.Queue(second)));
    Require(SUCCEEDED(pool.Ready(third)));
    Require(SUCCEEDED(pool.Upload(third)));
    Require(FAILED(pool.Queue(third)));
    Require(FAILED(pool.Latch(first))); // An old notifier cannot retire the newer update.
    Require(pool.Get(0) == FrameSlots::State::Front);
    Require(pool.Get(first) == FrameSlots::State::Queued);
    Require(SUCCEEDED(pool.Latch(second)));
    Require(pool.Get(second) == FrameSlots::State::Front);
    Require(pool.Get(third) == FrameSlots::State::Uploading);
    Require(SUCCEEDED(pool.Queue(third)));
    Require(SUCCEEDED(pool.Latch(third)));
    Require(FAILED(pool.Latch(third)));

    std::mt19937 random(871);
    for (UINT count = 2; count <= 4; ++count)
    {
        DeferredDamage(count);
        IndependentDamage(count);
        FrameSlots slots;
        Require(SUCCEEDED(slots.Configure(count, count - 1)));
        UINT front = count - 1;
        for (UINT step = 0; step < 100000; ++step)
        {
            const UINT index = random() % count;
            switch (slots.Get(index))
            {
            case FrameSlots::State::Free:
            {
                const UINT acquired = slots.Acquire();
                Require(acquired != FrameSlots::Invalid && acquired != front);
                break;
            }
            case FrameSlots::State::Copying:
                Require(SUCCEEDED(slots.Ready(index)));
                break;
            case FrameSlots::State::Ready:
                Require(SUCCEEDED(slots.Upload(index)));
                break;
            case FrameSlots::State::Uploading:
                if (slots.Queued() < 2)
                    Require(SUCCEEDED(slots.Queue(index)));
                break;
            case FrameSlots::State::Queued:
                if (index == slots.Latest())
                {
                    Require(SUCCEEDED(slots.Latch(index)));
                    front = index;
                }
                else
                    Require(FAILED(slots.Latch(index)));
                break;
            case FrameSlots::State::Front:
                break;
            }
            UINT fronts = 0;
            for (UINT slot = 0; slot < count; ++slot)
                fronts += slots.Get(slot) == FrameSlots::State::Front;
            Require(fronts == 1 && slots.Get(front) == FrameSlots::State::Front);
            Require(slots.Queued() <= 2);
        }
    }
    std::puts("PASS: 900000 ownership/damage schedules, independent RAM/VRAM leases, coalescing, stale notifications, and active leases");
}
