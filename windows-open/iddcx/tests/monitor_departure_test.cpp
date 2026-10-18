#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <cstdint>

using NvU32 = std::uint32_t;
using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS DepartureFailure = -1;
#define NT_SUCCESS(status) ((status) >= 0)
constexpr int TRACE_LEVEL_INFORMATION = 4;
constexpr int TRACE_LEVEL_ERROR = 2;
constexpr int TRACE_DRIVER = 1;

namespace {


class Adapter;
class Output;
struct Monitor
{
    Adapter* owner = nullptr;
    Output* output = nullptr;
    NvU32 displayHandle = 0;
    bool fail = false;
    bool defer = false;
    bool destroyed = false;
    unsigned departures = 0;
};
using IDDCX_MONITOR = Monitor*;

struct DisplayState
{
    NvU32 displayHandle;
};

class Output
{
public:
    IDDCX_MONITOR Monitor() const noexcept { return monitor_; }
    const ::DisplayState& DisplayState() const noexcept { return state_; }
    void StopMonitorControl() noexcept { EXPECT_TRUE(false); }

    ::Monitor* monitor_;
    ::DisplayState state_;
};

void TraceEvents(int, int, const char*, NTSTATUS, NvU32 displayHandle)
{
    EXPECT_TRUE(displayHandle >= 1 && displayHandle <= 16);
}

void WdfObjectDelete(IDDCX_MONITOR)
{
    EXPECT_TRUE(false);
}

NTSTATUS IddCxMonitorDeparture(IDDCX_MONITOR monitor);

class Adapter
{
public:
    static constexpr NvU32 MaxOutputs = 16;
    Output* outputs_[MaxOutputs] = {};
    NvU32 outputCount_ = 0;

    NTSTATUS Depart(const bool* connected) noexcept
    {
        NTSTATUS status = STATUS_SUCCESS;
        // The runner inserts the unmodified production departure loop here.
        /* PRODUCTION_DEPARTURE_LOOP */
        return status;
    }

    void OnOutputDestroyed(Output* output) noexcept
    {
        /* PRODUCTION_OUTPUT_REMOVAL */
    }
};

void Destroy(Monitor& monitor)
{
    ASSERT_FALSE(monitor.destroyed);
    monitor.owner->OnOutputDestroyed(monitor.output);
    monitor.destroyed = true;
    DWORD oldProtection = 0;
    // A framework-owned context becomes inaccessible before departure returns.
    EXPECT_TRUE(VirtualProtect(monitor.output, 4096, PAGE_NOACCESS, &oldProtection) != FALSE);
}

NTSTATUS IddCxMonitorDeparture(IDDCX_MONITOR monitor)
{
    if (monitor == nullptr || monitor->destroyed)
    {
        ADD_FAILURE() << "Departure requires a live monitor";
        return DepartureFailure;
    }
    EXPECT_TRUE(++monitor->departures == 1);
    if (monitor->fail)
    {
        return DepartureFailure;
    }
    if (!monitor->defer)
    {
        Destroy(*monitor);
    }
    return STATUS_SUCCESS;
}

void VerifyDepartureScenario(
    NvU32 count, NvU32 connectedMask, NvU32 failureMask, NvU32 deferredMask)
{
    Adapter adapter;
    std::array<Monitor, Adapter::MaxOutputs> monitors{};
    bool connected[Adapter::MaxOutputs] = {};
    NvU32 expectedRemaining = 0;
    bool expectFailure = false;
    for (NvU32 index = 0; index < count; ++index)
    {
        auto& monitor = monitors[index];
        monitor.owner = &adapter;
        monitor.displayHandle = index + 1;
        monitor.fail = (failureMask & (1U << index)) != 0;
        monitor.defer = (deferredMask & (1U << index)) != 0;
        monitor.output = static_cast<Output*>(VirtualAlloc(
            nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ASSERT_NE(monitor.output, nullptr);
        monitor.output->monitor_ = &monitor;
        monitor.output->state_.displayHandle = monitor.displayHandle;
        adapter.outputs_[adapter.outputCount_++] = monitor.output;
        connected[index] = (connectedMask & (1U << index)) != 0;
        expectedRemaining += connected[index] || monitor.fail;
        expectFailure = expectFailure || (!connected[index] && monitor.fail);
    }

    EXPECT_TRUE(adapter.Depart(connected) == (expectFailure ? DepartureFailure : STATUS_SUCCESS));
    EXPECT_TRUE(adapter.outputCount_ == expectedRemaining);
    NvU32 remainingIndex = 0;
    for (NvU32 index = 0; index < count; ++index)
    {
        auto& monitor = monitors[index];
        EXPECT_TRUE(monitor.departures == (connected[index] ? 0U : 1U));
        if (connected[index] || monitor.fail)
        {
            EXPECT_TRUE(!monitor.destroyed);
            EXPECT_TRUE(adapter.outputs_[remainingIndex++] == monitor.output);
        }
        else
        {
            if (monitor.defer)
            {
                EXPECT_TRUE(!monitor.destroyed);
                Destroy(monitor);
            }
            EXPECT_TRUE(monitor.destroyed);
            EXPECT_TRUE(adapter.outputCount_ == expectedRemaining);
        }
    }
    for (NvU32 index = adapter.outputCount_; index < Adapter::MaxOutputs; ++index)
    {
        EXPECT_TRUE(adapter.outputs_[index] == nullptr);
    }
    for (NvU32 index = 0; index < count; ++index)
    {
        EXPECT_TRUE(VirtualFree(monitors[index].output, 0, MEM_RELEASE) != FALSE);
    }
}

} // namespace

TEST(MonitorDeparture, EmptyAdapter)
{
    VerifyDepartureScenario(0, 0, 0, 0);
}

TEST(MonitorDeparture, ExhaustsFourOutputConnectionFailureAndDeferralMasks)
{
    // Synchronous destruction compacts outputs during iteration. Mix delayed
    // callbacks and failures to catch skipped or double-departed monitors.
    for (NvU32 connected = 0; connected < 16; ++connected)
    {
        for (NvU32 failed = 0; failed < 16; ++failed)
        {
            for (NvU32 deferred = 0; deferred < 16; ++deferred)
            {
                VerifyDepartureScenario(4, connected, failed, deferred);
            }
        }
    }
}

TEST(MonitorDeparture, SynchronouslyDepartsAllSixteenOutputs) { VerifyDepartureScenario(16, 0, 0, 0); }
TEST(MonitorDeparture, DefersAllSixteenDepartures) { VerifyDepartureScenario(16, 0, 0, 0xffff); }
TEST(MonitorDeparture, MixesSixteenConnectedFailedAndDeferredOutputs) { VerifyDepartureScenario(16, 0xaaaa, 0x4444, 0x2222); }
TEST(MonitorDeparture, LeavesAllConnectedOutputsAlive) { VerifyDepartureScenario(16, 0xffff, 0, 0); }
