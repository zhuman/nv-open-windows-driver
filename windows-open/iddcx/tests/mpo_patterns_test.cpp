#include "..\mpo_patterns.hpp"
#include "..\frame_slots.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Require(bool condition)
{
    if (!condition) { std::fputs("MPO pattern test failed\n", stderr); std::abort(); }
}
std::uint64_t Hash(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}
}

int main()
{
    nv::iddcx::PlaneBufferSlots slots[3];
    std::uint64_t random = 0x41ef63bc;
    for (unsigned tick = 0; tick < 600000; ++tick)
    {
        random = random * 6364136223846793005ULL + 1;
        const unsigned index = static_cast<unsigned>(random >> 32) % 3;
        auto& plane = slots[index];
        if (plane.Queued() != nv::iddcx::FrameSlots::Invalid)
        {
            Require(plane.Acquire() == nv::iddcx::FrameSlots::Invalid);
            const UINT queued = plane.Queued();
            Require(SUCCEEDED(plane.Latch()) && plane.Front() == queued);
        }
        else
        {
            const UINT front = plane.Front();
            const bool reuse = (random & 1) && front != nv::iddcx::FrameSlots::Invalid;
            const UINT writable = plane.Acquire(reuse);
            Require(writable < 3 && (reuse ? writable == front : writable != front));
            Require(plane.Acquire() == nv::iddcx::FrameSlots::Invalid);
            Require(SUCCEEDED(plane.Queue()));
            Require(plane.Front() == front);
            Require(plane.Acquire() == nv::iddcx::FrameSlots::Invalid);
            auto& independent = slots[(index + 1) % 3];
            if (independent.Queued() != nv::iddcx::FrameSlots::Invalid)
                Require(SUCCEEDED(independent.Latch()));
            Require(plane.Queued() == writable && plane.Front() == front);
        }
    }
    std::puts("PASS: 600000 independent MPO plane leases, front reuse and delayed per-layer retirement");
    namespace mpo = nv::iddcx::mpo;
    for (const unsigned hz : {15U, 30U, 60U})
    {
        mpo::FrameCadence cadence;
        Require(cadence.Configure(hz));
        const auto interval = (mpo::FrameCadence::TicksPerSecond + hz - 1) / hz;
        constexpr std::uint64_t start = 100000000;
        Require(cadence.Due(start) && cadence.Remaining(start) == 1);
        Require(cadence.Submitted(start));
        Require(!cadence.Due(start + interval - 1));
        Require(cadence.Remaining(start + interval - 1) == 1);
        Require(!cadence.Submitted(start + interval - 1));
        for (unsigned frame = 1; frame <= 3600; ++frame)
        {
            const auto target = start + frame * interval;
            const auto late = target + interval / 8;
            Require(cadence.Due(target));
            Require(cadence.Submitted(late));
            Require(cadence.Remaining(late) == interval - interval / 8);
            Require(!cadence.Due(start + (frame + 1) * interval - 1));
        }
        const auto stalled = start + 3610 * interval + interval / 2;
        Require(cadence.Submitted(stalled));
        Require(cadence.Remaining(stalled) == interval - interval / 2);
        Require(!cadence.Due(stalled));
        cadence.Reset();
        Require(cadence.Due(start));
        Require(cadence.Submitted(start));
        Require(!cadence.Submitted((std::numeric_limits<std::uint64_t>::max)()));
    }
    mpo::FrameCadence invalidCadence;
    Require(!invalidCadence.Configure(0) && !invalidCadence.Configure(25));
    Require(!invalidCadence.Submitted(100));
    std::puts("PASS: 15/30/60 Hz deadlines, bounded late-work drift, missed-slot skipping and restart");
    constexpr unsigned pitch = mpo::Width * 4 + 16;
    std::vector<std::uint8_t> image(pitch * mpo::Height + 64, 0xa5);
    for (unsigned mode = 1; mode <= 6; ++mode)
    {
        const auto first = mpo::Animate(mode, 0, 100);
        Require(mpo::Render(image, pitch, mode, first, 0, 0));
        const auto hash = Hash(image);
        Require(mpo::Render(image, pitch, mode, mpo::Animate(mode, 1, 100), 1, 0));
        Require(hash != Hash(image));
        for (unsigned y = 0; y < mpo::Height; ++y)
            for (unsigned x = mpo::Width * 4; x < pitch; ++x)
                Require(image[y * pitch + x] == 0xa5);
        for (unsigned index = pitch * mpo::Height; index < image.size(); ++index)
            Require(image[index] == 0xa5);
    }
    Require(!mpo::Render({image.data(), 16}, pitch, 1, {}, 0, 0));
    Require(!mpo::Render(image, mpo::Width * 4 - 1, 1, {}, 0, 0));
    for (unsigned curve = 0; curve < mpo::LutCount; ++curve)
    {
        float previous = 0;
        for (unsigned index = 0; index <= 1024; ++index)
        {
            const float value = mpo::Curve(curve, static_cast<float>(index) / 1024);
            Require(std::isfinite(value) && value >= previous && value <= 1);
            previous = value;
        }
        Require(previous == 1);
    }
    unsigned rotationMask = 0;
    unsigned showcaseMask = 0;
    for (unsigned tick = 0; tick < 7200; ++tick)
    {
        const double seconds = static_cast<double>(tick) / 60;
        const auto geometry = mpo::Animate(2, seconds, 100);
        Require(geometry.rotation % 90 == 0 && geometry.rotation <= 270);
        rotationMask |= 1U << (geometry.rotation / 90);
        const auto showcase = mpo::Animate(6, seconds, 100);
        showcaseMask |= 1U << showcase.effect;
        Require(showcase.effect != 1 && showcase.effect != 6 ?
            showcase.opacity == 255 : showcase.opacity >= 64 && showcase.opacity <= 224);
        Require(showcase.gain >= 0.25F && showcase.gain <= 1.5F);
        Require(showcase.scale >= 0.5F && showcase.scale <= 1.5F);
    }
    Require(rotationMask == 15 && showcaseMask == 126);
    for (unsigned mask = 1; mask <= 255; ++mask)
    {
        unsigned observed = 0;
        for (unsigned tick = 0; tick < 120; ++tick)
        {
            const auto geometry = mpo::Animate(2, tick * 0.25, 100, 0,
                mpo::AllFeatures, mask);
            const unsigned transform = geometry.rotation / 90 + (geometry.reflection ? 4 : 0);
            Require((mask & (1U << transform)) != 0);
            observed |= 1U << transform;
            const auto combined = mpo::Animate(6, 60 + tick * 0.05, 100, 0,
                mpo::AllFeatures, mask);
            Require(mask & (1U << (combined.rotation / 90 + (combined.reflection ? 4 : 0))));
        }
        Require(observed == mask);
    }
    for (unsigned mask = 1; mask <= 7; ++mask)
        for (unsigned tick = 0; tick < 120; ++tick)
        {
            const auto geometry = mpo::Animate(2, tick * 0.25, 100, 0,
                mpo::AllFeatures, 0x55, mask);
            const unsigned scale = geometry.scale == 0.5F ? 0 : geometry.scale == 1 ? 1 : 2;
            Require(mask & (1U << scale));
            const auto combined = mpo::Animate(6, 60 + tick * 0.05, 100, 0,
                mpo::AllFeatures, 0x55, mask);
            Require(combined.scale == 1);
        }
    for (unsigned features = 0; features <= mpo::AllFeatures; ++features)
        for (unsigned chapter = 0; chapter < 6; ++chapter)
        {
            const auto animation = mpo::Animate(6, chapter * 12, 100, 0, features);
            Require(animation.effect == 6 || (features & (1U << animation.effect)));
            if (!(features & (1U << 1))) Require(!animation.pixelAlpha && animation.opacity == 255);
            if (!(features & (1U << 2))) Require(animation.rotation == 0 && !animation.reflection && animation.scale == 1);
            if (!(features & (1U << 3))) Require(!animation.csc);
            if (!(features & (1U << 4))) Require(animation.gain == 1);
            if (!(features & (1U << 5))) Require(animation.lut == 0);
        }
    const auto gain = mpo::ColorMatrix(mpo::Animate(4, 0, 100));
    Require(gain[0][0] == 0.25F && gain[1][1] == 0.25F && gain[2][2] == 0.25F);
    Require(gain[0][1] == 0 && gain[0][3] == 0);
    std::puts("PASS: six live patterns, pitch/extent guards, hardware-animation states and five monotonic LUTs");
}
