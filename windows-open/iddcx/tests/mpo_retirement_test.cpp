#include <windows.h>
#include "../frame_slots.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using NvBool = unsigned;
using NvU32 = std::uint32_t;
using NvU64 = std::uint64_t;
using NV_STATUS = int;
constexpr NV_STATUS NV_OK = 0;
constexpr NvBool NV_FALSE = 0;
constexpr int TRACE_LEVEL_VERBOSE = 5;
constexpr int TRACE_DRIVER = 1;

namespace {
unsigned checks = 0;
unsigned queries = 0;
NvU32 failLayer = 0;
NvU32 failTimestampLayer = 0;
std::array<bool, 8> layerBusy{};
void Check(bool condition)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "MPO retirement case %u failed\n", checks);
        std::abort();
    }
}
NV_STATUS NvKmsWindowsGetLayerFlipPending(NvU32 head, NvU32 layer, NvBool* pending)
{
    Check(head == 3 && layer >= 1 && layer <= 7);
    ++queries;
    if (layer == failLayer) return -1;
    *pending = layerBusy[layer];
    return NV_OK;
}
NV_STATUS NvKmsWindowsGetLayerFlipTimestamp(NvU32 head, NvU32 layer, NvU64* timestamp)
{
    Check(head == 3 && !layerBusy[layer]);
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

int main()
{
    Scene scene;
    bool pending = true;
    layerBusy.fill(true);
    Check(scene.Retire(&pending) == S_OK && !pending && queries == 0);
    for (auto& plane : scene.planes_)
        Check(plane.slots.Acquire() != Scene::FrameInvalid);
    // Preparation alone has not armed any hardware flip notifier.
    Check(scene.Retire(&pending) == S_OK && !pending && queries == 0);
    for (auto& plane : scene.planes_) Check(plane.slots.Queue() == S_OK);
    Check(scene.Retire(&pending) == S_OK && pending && queries == 7);
    for (auto& plane : scene.planes_) Check(plane.slots.Front() == Scene::FrameInvalid);
    layerBusy[1] = false;
    Check(scene.Retire(&pending) == S_OK && pending);
    Check(scene.planes_[0].slots.Front() == 0 && scene.planes_[0].timestamp == 1001);
    layerBusy.fill(false);
    Check(scene.Retire(&pending) == S_OK && !pending);
    for (unsigned index = 0; index < scene.planes_.size(); ++index)
    {
        Check(scene.planes_[index].slots.Front() == 0);
        Check(scene.planes_[index].timestamp == 1001 + index);
    }
    // Once submitted, raw notifier state also covers later atomic layer disable.
    layerBusy[2] = true;
    Check(scene.Retire(&pending) == S_OK && pending);
    layerBusy.fill(false);
    failLayer = 3;
    Check(scene.Retire(&pending) == E_FAIL);
    failLayer = 0;
    for (auto& plane : scene.planes_)
    {
        Check(plane.slots.Acquire(true) == plane.slots.Front());
        Check(plane.slots.Queue() == S_OK);
    }
    failTimestampLayer = 2;
    Check(scene.Retire(&pending) == E_FAIL);
    Check(scene.planes_[1].slots.Queued() != Scene::FrameInvalid);
    failTimestampLayer = 0;
    Check(scene.Retire(&pending) == S_OK && !pending);
    for (auto& plane : scene.planes_)
        Check(plane.slots.Front() == 0 && plane.slots.Queued() == Scene::FrameInvalid);
    std::printf("%u production MPO notifier retirement checks passed\n", checks);
}
