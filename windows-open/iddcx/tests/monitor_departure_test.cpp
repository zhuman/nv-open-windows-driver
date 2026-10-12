#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using NvU32 = std::uint32_t;
using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS DepartureFailure = -1;
#define NT_SUCCESS(status) ((status) >= 0)
constexpr int TRACE_LEVEL_INFORMATION = 4;
constexpr int TRACE_LEVEL_ERROR = 2;
constexpr int TRACE_DRIVER = 1;

namespace {

unsigned checks = 0;
void Check(bool condition)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "Monitor departure case %u failed\n", checks);
        std::abort();
    }
}

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
    void StopMonitorControl() noexcept { Check(false); }

    ::Monitor* monitor_;
    ::DisplayState state_;
};

void TraceEvents(int, int, const char*, NTSTATUS, NvU32 displayHandle)
{
    Check(displayHandle >= 1 && displayHandle <= 16);
}

void WdfObjectDelete(IDDCX_MONITOR)
{
    Check(false);
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
    Check(!monitor.destroyed);
    monitor.owner->OnOutputDestroyed(monitor.output);
    monitor.destroyed = true;
    DWORD oldProtection = 0;
    // A framework-owned context becomes inaccessible before departure returns.
    Check(VirtualProtect(monitor.output, 4096, PAGE_NOACCESS, &oldProtection) != FALSE);
}

NTSTATUS IddCxMonitorDeparture(IDDCX_MONITOR monitor)
{
    Check(!monitor->destroyed);
    Check(++monitor->departures == 1);
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

void Run(NvU32 count, NvU32 connectedMask, NvU32 failureMask, NvU32 deferredMask)
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
        Check(monitor.output != nullptr);
        monitor.output->monitor_ = &monitor;
        monitor.output->state_.displayHandle = monitor.displayHandle;
        adapter.outputs_[adapter.outputCount_++] = monitor.output;
        connected[index] = (connectedMask & (1U << index)) != 0;
        expectedRemaining += connected[index] || monitor.fail;
        expectFailure = expectFailure || (!connected[index] && monitor.fail);
    }

    Check(adapter.Depart(connected) == (expectFailure ? DepartureFailure : STATUS_SUCCESS));
    Check(adapter.outputCount_ == expectedRemaining);
    NvU32 remainingIndex = 0;
    for (NvU32 index = 0; index < count; ++index)
    {
        auto& monitor = monitors[index];
        Check(monitor.departures == (connected[index] ? 0U : 1U));
        if (connected[index] || monitor.fail)
        {
            Check(!monitor.destroyed);
            Check(adapter.outputs_[remainingIndex++] == monitor.output);
        }
        else
        {
            if (monitor.defer)
            {
                Check(!monitor.destroyed);
                Destroy(monitor);
            }
            Check(monitor.destroyed);
            Check(adapter.outputCount_ == expectedRemaining);
        }
    }
    for (NvU32 index = adapter.outputCount_; index < Adapter::MaxOutputs; ++index)
    {
        Check(adapter.outputs_[index] == nullptr);
    }
    for (NvU32 index = 0; index < count; ++index)
    {
        Check(VirtualFree(monitors[index].output, 0, MEM_RELEASE) != FALSE);
    }
}

} // namespace

int main()
{
    Run(0, 0, 0, 0);
    for (NvU32 connected = 0; connected < 16; ++connected)
    {
        for (NvU32 failed = 0; failed < 16; ++failed)
        {
            for (NvU32 deferred = 0; deferred < 16; ++deferred)
            {
                Run(4, connected, failed, deferred);
            }
        }
    }
    Run(16, 0, 0, 0);
    Run(16, 0, 0, 0xffff);
    Run(16, 0xaaaa, 0x4444, 0x2222);
    Run(16, 0xffff, 0, 0);
    std::printf("%u production monitor departure checks passed\n", checks);
}
