#include <gtest/gtest.h>

#include "..\mpo_patterns.hpp"
#include "..\frame_slots.hpp"
#include <vector>

namespace {
std::uint64_t Hash(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
}
}

TEST(MpoPatterns, ModelsIndependentPlaneLeasesAndDelayedRetirement)
{
    // Retiring one plane must not release a queued or front lease on a sibling.
    nv::iddcx::PlaneBufferSlots slots[3];
    std::uint64_t random = 0x41ef63bc;
    for (unsigned tick = 0; tick < 600000; ++tick)
    {
        random = random * 6364136223846793005ULL + 1;
        const unsigned index = static_cast<unsigned>(random >> 32) % 3;
        auto& plane = slots[index];
        if (plane.Queued() != nv::iddcx::FrameSlots::Invalid)
        {
            EXPECT_TRUE(plane.Acquire() == nv::iddcx::FrameSlots::Invalid);
            const UINT queued = plane.Queued();
            EXPECT_TRUE(SUCCEEDED(plane.Latch()) && plane.Front() == queued);
        }
        else
        {
            const UINT front = plane.Front();
            const bool reuse = (random & 1) && front != nv::iddcx::FrameSlots::Invalid;
            const UINT writable = plane.Acquire(reuse);
            EXPECT_TRUE(writable < 3 && (reuse ? writable == front : writable != front));
            EXPECT_TRUE(plane.Acquire() == nv::iddcx::FrameSlots::Invalid);
            EXPECT_TRUE(SUCCEEDED(plane.Queue()));
            EXPECT_TRUE(plane.Front() == front);
            EXPECT_TRUE(plane.Acquire() == nv::iddcx::FrameSlots::Invalid);
            auto& independent = slots[(index + 1) % 3];
            if (independent.Queued() != nv::iddcx::FrameSlots::Invalid)
                EXPECT_TRUE(SUCCEEDED(independent.Latch()));
            EXPECT_TRUE(plane.Queued() == writable && plane.Front() == front);
        }
    }
}

TEST(MpoPatterns, MaintainsCadenceDeadlinesAcrossLateWorkStallsAndRestart)
{
    // Lateness must not shift the cadence phase; stalls skip elapsed slots
    // rather than scheduling a burst of catch-up frames.
    namespace mpo = nv::iddcx::mpo;
    for (const unsigned hz : {15U, 30U, 60U})
    {
        mpo::FrameCadence cadence;
        EXPECT_TRUE(cadence.Configure(hz));
        const auto interval = (mpo::FrameCadence::TicksPerSecond + hz - 1) / hz;
        constexpr std::uint64_t start = 100000000;
        EXPECT_TRUE(cadence.Due(start) && cadence.Remaining(start) == 1);
        EXPECT_TRUE(cadence.Submitted(start));
        EXPECT_TRUE(!cadence.Due(start + interval - 1));
        EXPECT_TRUE(cadence.Remaining(start + interval - 1) == 1);
        EXPECT_TRUE(!cadence.Submitted(start + interval - 1));
        for (unsigned frame = 1; frame <= 3600; ++frame)
        {
            const auto target = start + frame * interval;
            const auto late = target + interval / 8;
            EXPECT_TRUE(cadence.Due(target));
            EXPECT_TRUE(cadence.Submitted(late));
            EXPECT_TRUE(cadence.Remaining(late) == interval - interval / 8);
            EXPECT_TRUE(!cadence.Due(start + (frame + 1) * interval - 1));
        }
        const auto stalled = start + 3610 * interval + interval / 2;
        EXPECT_TRUE(cadence.Submitted(stalled));
        EXPECT_TRUE(cadence.Remaining(stalled) == interval - interval / 2);
        EXPECT_TRUE(!cadence.Due(stalled));
        cadence.Reset();
        EXPECT_TRUE(cadence.Due(start));
        EXPECT_TRUE(cadence.Submitted(start));
        EXPECT_TRUE(!cadence.Submitted((std::numeric_limits<std::uint64_t>::max)()));
    }
    mpo::FrameCadence invalidCadence;
    EXPECT_TRUE(!invalidCadence.Configure(0) && !invalidCadence.Configure(25));
    EXPECT_TRUE(!invalidCadence.Submitted(100));
}

TEST(MpoPatterns, RendersSixPatternsWithoutTouchingPaddingOrExtent)
{
    // Sentinels catch writes beyond visible pixels. Hash changes establish
    // animation, not visual correctness of the rendered scene.
    namespace mpo = nv::iddcx::mpo;
    constexpr unsigned pitch = mpo::Width * 4 + 16;
    std::vector<std::uint8_t> image(pitch * mpo::Height + 64, 0xa5);
    for (unsigned mode = 1; mode <= 6; ++mode)
    {
        const auto first = mpo::Animate(mode, 0, 100);
        EXPECT_TRUE(mpo::Render(image, pitch, mode, first, 0, 0));
        const auto hash = Hash(image);
        EXPECT_TRUE(mpo::Render(image, pitch, mode, mpo::Animate(mode, 1, 100), 1, 0));
        EXPECT_TRUE(hash != Hash(image));
        for (unsigned y = 0; y < mpo::Height; ++y)
            for (unsigned x = mpo::Width * 4; x < pitch; ++x)
                EXPECT_TRUE(image[y * pitch + x] == 0xa5);
        for (unsigned index = pitch * mpo::Height; index < image.size(); ++index)
            EXPECT_TRUE(image[index] == 0xa5);
    }
    EXPECT_TRUE(!mpo::Render({image.data(), 16}, pitch, 1, {}, 0, 0));
    EXPECT_TRUE(!mpo::Render(image, mpo::Width * 4 - 1, 1, {}, 0, 0));
}

TEST(MpoPatterns, ProducesFiniteMonotonicLutCurves)
{
    namespace mpo = nv::iddcx::mpo;
    for (unsigned curve = 0; curve < mpo::LutCount; ++curve)
    {
        float previous = 0;
        for (unsigned index = 0; index <= 1024; ++index)
        {
            const float value = mpo::Curve(curve, static_cast<float>(index) / 1024);
            EXPECT_TRUE(std::isfinite(value) && value >= previous && value <= 1);
            previous = value;
        }
        EXPECT_TRUE(previous == 1);
    }
}

TEST(MpoPatterns, AnimatesOnlySupportedTransformsScalesAndFeatures)
{
    namespace mpo = nv::iddcx::mpo;
    unsigned rotationMask = 0;
    unsigned showcaseMask = 0;
    for (unsigned tick = 0; tick < 7200; ++tick)
    {
        const double seconds = static_cast<double>(tick) / 60;
        const auto geometry = mpo::Animate(2, seconds, 100);
        EXPECT_TRUE(geometry.rotation % 90 == 0 && geometry.rotation <= 270);
        rotationMask |= 1U << (geometry.rotation / 90);
        const auto showcase = mpo::Animate(6, seconds, 100);
        showcaseMask |= 1U << showcase.effect;
        EXPECT_TRUE(showcase.effect != 1 && showcase.effect != 6 ?
            showcase.opacity == 255 : showcase.opacity >= 64 && showcase.opacity <= 224);
        EXPECT_TRUE(showcase.gain >= 0.25F && showcase.gain <= 1.5F);
        EXPECT_TRUE(showcase.scale >= 0.5F && showcase.scale <= 1.5F);
    }
    EXPECT_TRUE(rotationMask == 15 && showcaseMask == 126);
    // Bits 0-3 encode quarter turns, bits 4-7 their reflected variants.
    // Sweep capability subsets so animation cannot choose an unsupported pose.
    for (unsigned mask = 1; mask <= 255; ++mask)
    {
        unsigned observed = 0;
        for (unsigned tick = 0; tick < 120; ++tick)
        {
            const auto geometry = mpo::Animate(2, tick * 0.25, 100, 0,
                mpo::AllFeatures, mask);
            const unsigned transform = geometry.rotation / 90 + (geometry.reflection ? 4 : 0);
            EXPECT_TRUE((mask & (1U << transform)) != 0);
            observed |= 1U << transform;
            const auto combined = mpo::Animate(6, 60 + tick * 0.05, 100, 0,
                mpo::AllFeatures, mask);
            EXPECT_TRUE(mask & (1U << (combined.rotation / 90 + (combined.reflection ? 4 : 0))));
        }
        EXPECT_TRUE(observed == mask);
    }
    for (unsigned mask = 1; mask <= 7; ++mask)
        for (unsigned tick = 0; tick < 120; ++tick)
        {
            const auto geometry = mpo::Animate(2, tick * 0.25, 100, 0,
                mpo::AllFeatures, 0x55, mask);
            const unsigned scale = geometry.scale == 0.5F ? 0 : geometry.scale == 1 ? 1 : 2;
            EXPECT_TRUE(mask & (1U << scale));
            // The combined CSC/gain chapter already consumes the scaling path.
            const auto combined = mpo::Animate(6, 60 + tick * 0.05, 100, 0,
                mpo::AllFeatures, 0x55, mask);
            EXPECT_TRUE(combined.scale == 1);
        }
    for (unsigned features = 0; features <= mpo::AllFeatures; ++features)
        for (unsigned chapter = 0; chapter < 6; ++chapter)
        {
            const auto animation = mpo::Animate(6, chapter * 12, 100, 0, features);
            EXPECT_TRUE(animation.effect == 6 || (features & (1U << animation.effect)));
            if (!(features & (1U << 1))) EXPECT_TRUE(!animation.pixelAlpha && animation.opacity == 255);
            if (!(features & (1U << 2))) EXPECT_TRUE(animation.rotation == 0 && !animation.reflection && animation.scale == 1);
            if (!(features & (1U << 3))) EXPECT_TRUE(!animation.csc);
            if (!(features & (1U << 4))) EXPECT_TRUE(animation.gain == 1);
            if (!(features & (1U << 5))) EXPECT_TRUE(animation.lut == 0);
        }
    const auto gain = mpo::ColorMatrix(mpo::Animate(4, 0, 100));
    EXPECT_TRUE(gain[0][0] == 0.25F && gain[1][1] == 0.25F && gain[2][2] == 0.25F);
    EXPECT_TRUE(gain[0][1] == 0 && gain[0][3] == 0);
}
