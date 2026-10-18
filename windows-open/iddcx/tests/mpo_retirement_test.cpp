#include <gtest/gtest.h>

#include <windows.h>
#include "../frame_slots.hpp"
#include <array>
#include <cstdint>

using NvBool = unsigned;
using NvU32 = std::uint32_t;
using NvU64 = std::uint64_t;
using NV_STATUS = int;
constexpr NV_STATUS NV_OK = 0;
constexpr NvBool NV_FALSE = 0;
constexpr int TRACE_LEVEL_VERBOSE = 5;
constexpr int TRACE_DRIVER = 1;

namespace {
unsigned queries = 0;
NvU32 failLayer = 0;
NvU32 failTimestampLayer = 0;
std::array<bool, 8> layerBusy{};
NV_STATUS NvKmsWindowsGetLayerFlipPending(NvU32 head, NvU32 layer, NvBool* pending)
{
    if (head != 3 || layer < 1 || layer > 7)
    {
        ADD_FAILURE() << "Unexpected flip-pending head or layer";
        return -1;
    }
    ++queries;
    if (layer == failLayer) return -1;
    *pending = layerBusy[layer];
    return NV_OK;
}
NV_STATUS NvKmsWindowsGetLayerFlipTimestamp(NvU32 head, NvU32 layer, NvU64* timestamp)
{
    if (head != 3 || layer < 1 || layer > 7)
    {
        ADD_FAILURE() << "Unexpected timestamp head or layer";
        return -1;
    }
    EXPECT_FALSE(layerBusy[layer]);
    if (layer == failTimestampLayer) return -1;
    *timestamp = 1000 + layer;
    return NV_OK;
}
void TraceEvents(int, int, const char*, NvU32, unsigned, UINT, NvU64) {}
struct Output
{
    NvU32 AssignedHead() const { return 3; }
};
class Scene
{
public:
    HRESULT Retire(bool* pending) noexcept
    {
        /* PRODUCTION_RETIREMENT */
    }
    static constexpr UINT FrameInvalid = ~UINT{0};
    struct Plane
    {
        nv::iddcx::PlaneBufferSlots slots;
        NvU64 timestamp = 0;
    };
    struct Config { NvU32 demoPlanes = 7; NvU32 connector = 3; } config_;
    Output output;
    Output* output_ = &output;
    std::array<Plane, 7> planes_;
};
}

class MpoRetirement : public ::testing::Test
{
protected:
    void SetUp() override
    {
        queries = 0;
        failLayer = failTimestampLayer = 0;
        layerBusy.fill(true);
    }
    void QueueAllPlanes()
    {
        for (auto& plane : scene.planes_)
        {
            ASSERT_NE(plane.slots.Acquire(), Scene::FrameInvalid);
            ASSERT_EQ(plane.slots.Queue(), S_OK);
        }
    }
    void LatchAllPlanes()
    {
        ASSERT_NO_FATAL_FAILURE(QueueAllPlanes());
        layerBusy.fill(false);
        ASSERT_EQ(scene.Retire(&pending), S_OK);
        ASSERT_FALSE(pending);
    }
    Scene scene;
    bool pending = true;
};

TEST_F(MpoRetirement, DoesNotQueryNotifiersUntilQueued)
{
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && !pending && queries == 0);
    for (auto& plane : scene.planes_)
        EXPECT_TRUE(plane.slots.Acquire() != Scene::FrameInvalid);
    // Preparation alone has not armed any hardware flip notifier.
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && !pending && queries == 0);
}

TEST_F(MpoRetirement, RetiresLayersIndependentlyAndRecordsTimestamps)
{
    // A retired layer must not release its busy siblings through a head-wide proxy.
    ASSERT_NO_FATAL_FAILURE(QueueAllPlanes());
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && pending && queries == 7);
    for (auto& plane : scene.planes_) EXPECT_TRUE(plane.slots.Front() == Scene::FrameInvalid);
    layerBusy[1] = false;
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && pending);
    EXPECT_TRUE(scene.planes_[0].slots.Front() == 0 && scene.planes_[0].timestamp == 1001);
    layerBusy.fill(false);
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && !pending);
    for (unsigned index = 0; index < scene.planes_.size(); ++index)
    {
        EXPECT_TRUE(scene.planes_[index].slots.Front() == 0);
        EXPECT_TRUE(scene.planes_[index].timestamp == 1001 + index);
    }
}

TEST_F(MpoRetirement, SubmittedNotifierCoversLaterAtomicLayerDisable)
{
    ASSERT_NO_FATAL_FAILURE(LatchAllPlanes());
    // Once submitted, raw notifier state also covers later atomic layer disable.
    layerBusy[2] = true;
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && pending);
}

TEST_F(MpoRetirement, PropagatesPendingQueryFailure)
{
    ASSERT_NO_FATAL_FAILURE(LatchAllPlanes());
    failLayer = 3;
    EXPECT_TRUE(scene.Retire(&pending) == E_FAIL);
}

TEST_F(MpoRetirement, TimestampFailurePreservesQueuedLeaseForRetry)
{
    // A cleared pending bit is insufficient: retain ownership until its
    // timestamp is available, so an error cannot recycle the scanout buffer.
    ASSERT_NO_FATAL_FAILURE(LatchAllPlanes());
    for (auto& plane : scene.planes_)
    {
        EXPECT_TRUE(plane.slots.Acquire(true) == plane.slots.Front());
        EXPECT_TRUE(plane.slots.Queue() == S_OK);
    }
    failTimestampLayer = 2;
    EXPECT_TRUE(scene.Retire(&pending) == E_FAIL);
    EXPECT_TRUE(scene.planes_[1].slots.Queued() != Scene::FrameInvalid);
    failTimestampLayer = 0;
    EXPECT_TRUE(scene.Retire(&pending) == S_OK && !pending);
    for (auto& plane : scene.planes_)
        EXPECT_TRUE(plane.slots.Front() == 0 && plane.slots.Queued() == Scene::FrameInvalid);
}
