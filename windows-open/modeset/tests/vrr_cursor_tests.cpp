#include "nvkms-vrr.h"
#include <gtest/gtest.h>

#include <memory>
#include <cstring>
#include <vector>

extern "C" void NvTestClearApiHeadStateOneDisp(NVDispEvoPtr);

struct nvkms_timer_t
{
    nvkms_timer_proc_t* callback;
    void* context;
    NvU32 head;
    NvU64 deadline;
    bool canceled = false;
    bool fired = false;
};

namespace {

NvU64 nowUsec = 0;
bool failTimerAllocation = false;
unsigned warnings = 0;
std::vector<std::unique_ptr<nvkms_timer_t>> timers;
std::vector<NvU32> releasedHeads;

void ReleaseElv(NVDevEvoPtr, NvU32 head)
{
    releasedHeads.push_back(head);
}

void AdvanceTo(NvU64 deadline)
{
    nowUsec = deadline;
    for (const auto& timer : timers)
    {
        if (!timer->canceled && !timer->fired && timer->deadline <= nowUsec)
        {
            timer->fired = true;
            timer->callback(timer->context, timer->head);
        }
    }
}

unsigned LiveTimers()
{
    unsigned count = 0;
    for (const auto& timer : timers)
        if (!timer->canceled) ++count;
    return count;
}

class VrrCursor : public ::testing::Test
{
protected:
    void SetUp() override
    {
        nowUsec = 1000000;
        failTimerAllocation = false;
        warnings = 0;
        timers.clear();
        releasedHeads.clear();
        device = std::make_unique<NVDevEvoRec>();
        display = std::make_unique<NVDispEvoRec>();
        connector = std::make_unique<NVConnectorEvoRec>();
        subdevice = std::make_unique<NVEvoSubDevRec>();
        device->numSubDevices = 1;
        device->nDispEvo = 1;
        device->pDispEvo[0] = display.get();
        device->numHeads = 3;
        device->numApiHeads = 2;
        device->gpus = subdevice.get();
        device->gpus[0].pDispEvo = display.get();
        cursorHal.ReleaseElv = &ReleaseElv;
        device->cursorHal = &cursorHal;
        display->pDevEvo = device.get();
        ConfigureHead(0, NVBIT(0), 120000);
        ConfigureHead(1, NVBIT(1), 60000);
    }

    void ConfigureHead(NvU32 apiHead, NvU32 hardwareHeads, NvU32 pixelClock)
    {
        auto& state = display->apiHeadState[apiHead];
        state.hwHeadsMask = hardwareHeads;
        state.activeDpys = nvAllDpyIdList();
        state.vrr.active = NV_TRUE;
        state.timings.rasterSize = {1000, 1000};
        state.timings.pixelClock = pixelClock;
        for (NvU32 head = 0; head < device->numHeads; ++head)
            if (hardwareHeads & NVBIT(head))
                display->headState[head].pConnectorEvo = connector.get();
    }

    void Flip(NvU32 heads = NVBIT(0) | NVBIT(1))
    {
        NvU32 masks[NVKMS_MAX_SUBDEVICES] = {};
        masks[0] = heads;
        nvCancelVrrFrameReleaseTimers(device.get(), masks);
    }

    struct _nv_evo_cursor_hal cursorHal = {};
    std::unique_ptr<NVDevEvoRec> device;
    std::unique_ptr<NVDispEvoRec> display;
    std::unique_ptr<NVConnectorEvoRec> connector;
    std::unique_ptr<NVEvoSubDevRec> subdevice;
};

} // namespace

extern "C" NvU64 nvkms_get_usec(void)
{
    return nowUsec;
}

extern "C" nvkms_timer_handle_t* nvkms_alloc_timer(
    nvkms_timer_proc_t* callback, void* context, NvU32 head, NvU64 delayUsec)
{
    if (failTimerAllocation) return nullptr;
    auto timer = std::make_unique<nvkms_timer_t>();
    timer->callback = callback;
    timer->context = context;
    timer->head = head;
    timer->deadline = nowUsec + delayUsec;
    timers.push_back(std::move(timer));
    return timers.back().get();
}

extern "C" void nvkms_free_timer(nvkms_timer_handle_t* timer)
{
    if (timer == nullptr) return;
    EXPECT_FALSE(timer->canceled) << "A cursor timer must be released exactly once";
    timer->canceled = true;
}

extern "C" void nvkms_log(int level, const char*, const char*)
{
    EXPECT_EQ(NVKMS_LOG_LEVEL_WARN, level);
    ++warnings;
}

extern "C" void UnregisterFlipOccurredEventOneHead(NVDispEvoPtr, NvU32) {}
extern "C" void nvkms_free_ref_ptr(nvkms_ref_ptr*)
{
    ADD_FAILURE() << "The cursor fixture must not own unrelated flip callbacks";
}
extern "C" void* nvkms_memset(void* destination, NvU8 value, size_t size)
{
    return std::memset(destination, value, size);
}

#if defined(NV_WINDOWS_IDDCX)

TEST_F(VrrCursor, IdleMotionReleasesOnlyTheAffectedHeadImmediately)
{
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    ASSERT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, releasedHeads[0]);
    EXPECT_EQ(0U, LiveTimers());
    EXPECT_FALSE(display->apiHeadState[0].vrr.pendingCursorMotion);
    EXPECT_EQ(nowUsec, display->apiHeadState[0].vrr.lastCursorFrameReleaseUsec);
}

TEST_F(VrrCursor, CoalescesMotionAtTheExact120HzBoundaryWithoutLosingTheFinalMove)
{
    Flip();
    nowUsec += 1000;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    ASSERT_EQ(1U, timers.size());
    EXPECT_EQ(1008334U, timers[0]->deadline);
    nowUsec += 6000;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1U, timers.size());
    // No further mouse event is needed to make the last programmed position visible.
    AdvanceTo(1008333);
    EXPECT_TRUE(releasedHeads.empty());
    AdvanceTo(1008334);
    ASSERT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, LiveTimers());
    EXPECT_FALSE(display->apiHeadState[0].vrr.pendingCursorMotion);
}

TEST_F(VrrCursor, UsesFractionalModeTimingInsteadOfAWholeHzOrFixed30HzDelay)
{
    const struct { NvU32 clock; NvU64 period; } cases[] = {
        {59940, 16684}, {60000, 16667}, {119880, 8342},
        {120000, 8334}, {144000, 6945}, {240000, 4167}
    };
    for (const auto& item : cases)
    {
        SCOPED_TRACE(item.clock);
        display->apiHeadState[0].timings.pixelClock = item.clock;
        Flip(NVBIT(0));
        const NvU64 lastFrame = nowUsec;
        ++nowUsec;
        nvTriggerVrrUnstallMoveCursor(display.get(), 0);
        ASSERT_FALSE(timers.empty());
        EXPECT_EQ(lastFrame + item.period, timers.back()->deadline);
        const size_t releases = releasedHeads.size();
        AdvanceTo(lastFrame + item.period - 1);
        EXPECT_EQ(releases, releasedHeads.size());
        AdvanceTo(lastFrame + item.period);
        EXPECT_EQ(releases + 1, releasedHeads.size());
    }
}

TEST_F(VrrCursor, PixelFlipsCancelRedundantRefreshAndRestartTheFrameInterval)
{
    Flip();
    nowUsec += 1000;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nowUsec += 3000;
    Flip(NVBIT(0));
    EXPECT_EQ(0U, LiveTimers());
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1012334U, timers.back()->deadline);
    AdvanceTo(1008334);
    EXPECT_TRUE(releasedHeads.empty());
    AdvanceTo(1012334);
    EXPECT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, LiveTimers());
}

TEST_F(VrrCursor, ContinuedMotionDoesNotWaitForAnOverdueWindowsTimer)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    // Model a timer callback delayed by scheduling while mouse events still arrive.
    nowUsec = 1008333;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_TRUE(releasedHeads.empty());
    nowUsec = 1008334;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, LiveTimers());
    AdvanceTo(1010000);
    EXPECT_EQ(1U, releasedHeads.size());
}

TEST_F(VrrCursor, LateTimerStartsANewIntervalInsteadOfCatchingUpMissedFrames)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    AdvanceTo(1050000);
    EXPECT_EQ(1U, releasedHeads.size());
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1058334U, timers.back()->deadline);
    EXPECT_EQ(1U, releasedHeads.size());
    AdvanceTo(1058334);
    EXPECT_EQ(2U, releasedHeads.size());
}

TEST_F(VrrCursor, MixedRefreshHeadsHaveIndependentDeadlinesAndCancellation)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nvTriggerVrrUnstallMoveCursor(display.get(), 1);
    ASSERT_EQ(2U, LiveTimers());
    AdvanceTo(1008334);
    ASSERT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, releasedHeads.back());
    Flip(NVBIT(0));
    EXPECT_EQ(1U, LiveTimers());
    AdvanceTo(1016667);
    ASSERT_EQ(2U, releasedHeads.size());
    EXPECT_EQ(1U, releasedHeads.back());
    EXPECT_EQ(0U, LiveTimers());
}

TEST_F(VrrCursor, SplitApiHeadReleasesEachActiveHardwareHeadOnlyOnce)
{
    ConfigureHead(0, NVBIT(0) | NVBIT(2), 120000);
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nvTriggerVrrUnstallMoveCursor(display.get(), 2);
    ASSERT_EQ(1U, LiveTimers());
    AdvanceTo(1008334);
    ASSERT_EQ(2U, releasedHeads.size());
    EXPECT_EQ(0U, releasedHeads[0]);
    EXPECT_EQ(2U, releasedHeads[1]);
}

TEST_F(VrrCursor, InactiveVrrAndUnmappedHeadsNeverRequestARefresh)
{
    display->apiHeadState[0].vrr.active = NV_FALSE;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nvTriggerVrrUnstallMoveCursor(display.get(), 2);
    nvTriggerVrrUnstallMoveCursor(display.get(), device->numHeads);
    EXPECT_TRUE(releasedHeads.empty());
    EXPECT_TRUE(timers.empty());
}

TEST_F(VrrCursor, DisabledHeadDoesNotTouchHardwareWhenItsOldDeadlineArrives)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    display->apiHeadState[0].vrr.active = NV_FALSE;
    AdvanceTo(1008334);
    EXPECT_TRUE(releasedHeads.empty());
    EXPECT_EQ(0U, LiveTimers());
}

TEST_F(VrrCursor, FullCancellationLeavesNoTimerAbleToAccessTheDisplay)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nvTriggerVrrUnstallMoveCursor(display.get(), 1);
    Flip(~0U);
    display.reset();
    AdvanceTo(2000000);
    EXPECT_TRUE(releasedHeads.empty());
    EXPECT_EQ(0U, LiveTimers());
}

TEST_F(VrrCursor, ClearingApiStateCancelsTimersBeforeZeroingTheirOwners)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nvTriggerVrrUnstallMoveCursor(display.get(), 1);
    NvTestClearApiHeadStateOneDisp(display.get());
    EXPECT_EQ(0U, LiveTimers());
    EXPECT_EQ(nullptr, display->apiHeadState[0].vrr.cursorUnstallTimer);
    EXPECT_EQ(nullptr, display->apiHeadState[1].vrr.cursorUnstallTimer);
    AdvanceTo(2000000);
    EXPECT_TRUE(releasedHeads.empty());
}

TEST_F(VrrCursor, CursorImageReleaseCancelsOnlyItsOwnPendingMotion)
{
    Flip();
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    nvTriggerVrrUnstallMoveCursor(display.get(), 1);
    // An image update already released ELV; recording it must not release twice.
    nvTriggerVrrUnstallSetCursorImage(display.get(), 0, NV_TRUE);
    EXPECT_TRUE(releasedHeads.empty());
    EXPECT_EQ(1U, LiveTimers());
    ++nowUsec;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1008335U, timers.back()->deadline);
    nvTriggerVrrUnstallSetCursorImage(display.get(), 0, NV_FALSE);
    ASSERT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, releasedHeads.back());
    EXPECT_EQ(1U, LiveTimers());
    Flip(~0U);
    EXPECT_EQ(0U, LiveTimers());
}

TEST_F(VrrCursor, TimerFailureIsReportedAndDoesNotStrandTheCursor)
{
    Flip();
    ++nowUsec;
    failTimerAllocation = true;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1U, warnings);
    EXPECT_EQ(1U, releasedHeads.size());
    EXPECT_EQ(0U, LiveTimers());
}

TEST_F(VrrCursor, SustainedMotionReleasesEachTimerInsteadOfAccumulatingHandles)
{
    Flip();
    for (unsigned frame = 0; frame < 1000; ++frame)
    {
        const NvU64 nextFrame = nowUsec + 8334;
        ++nowUsec;
        nvTriggerVrrUnstallMoveCursor(display.get(), 0);
        EXPECT_EQ(1U, LiveTimers());
        AdvanceTo(nextFrame);
        EXPECT_EQ(0U, LiveTimers());
    }
    EXPECT_EQ(1000U, releasedHeads.size());
}

#else

TEST_F(VrrCursor, NativePolicyKeepsTheOriginal33MillisecondFallback)
{
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    ASSERT_EQ(1U, timers.size());
    EXPECT_EQ(1033000U, timers[0]->deadline);
    nowUsec += 1000;
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    EXPECT_EQ(1U, timers.size());
    AdvanceTo(1032999);
    EXPECT_TRUE(releasedHeads.empty());
    AdvanceTo(1033000);
    EXPECT_EQ(2U, releasedHeads.size());
    EXPECT_EQ(nullptr, display->vrr.unstallTimer);
}

TEST_F(VrrCursor, NativePixelFlipsStillCancelTheSharedTimer)
{
    nvTriggerVrrUnstallMoveCursor(display.get(), 0);
    Flip();
    EXPECT_EQ(0U, LiveTimers());
    AdvanceTo(2000000);
    EXPECT_TRUE(releasedHeads.empty());
}

#endif
