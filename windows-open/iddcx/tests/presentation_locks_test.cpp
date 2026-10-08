#include "../../modeset/presentation_locks.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <type_traits>

namespace {

std::atomic<unsigned> cases{0};

void Check(bool condition)
{
    const auto current = ++cases;
    if (!condition)
    {
        std::fprintf(stderr, "Presentation concurrency case %u failed\n", current);
        std::abort();
    }
}

class Event
{
public:
    Event() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle)
    {
        Check(handle_ != nullptr);
    }
    void Signal() { Check(SetEvent(handle_.get()) != FALSE); }
    bool Wait(DWORD timeout = 5000)
    {
        const DWORD result = WaitForSingleObject(handle_.get(), timeout);
        Check(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT);
        return result == WAIT_OBJECT_0;
    }
private:
    std::unique_ptr<void, decltype(&CloseHandle)> handle_;
};

void TestCopyAndCursor()
{
    SRWLOCK device = SRWLOCK_INIT;
    nv::modeset::PresentationLocks locks(device);
    Event cursorDone;
    Event shutdownStarted;
    Event shutdownDone;
    std::thread shutdown;
    {
        auto lifetime = locks.DeviceShared();
        auto movedLifetime = std::move(lifetime);
        auto presenter = locks.Present();
        auto copy = locks.Copy();
        std::thread cursor([&] {
            auto cursorLifetime = locks.DeviceShared();
            auto programming = locks.Programming();
            cursorDone.Signal();
        });
        Check(cursorDone.Wait());
        cursor.join();

        shutdown = std::thread([&] {
            shutdownStarted.Signal();
            auto exclusive = locks.DeviceExclusive();
            shutdownDone.Signal();
        });
        Check(shutdownStarted.Wait());
        Check(!shutdownDone.Wait(50));
    }
    Check(shutdownDone.Wait());
    shutdown.join();
}

void TestProgrammingExclusion()
{
    SRWLOCK device = SRWLOCK_INIT;
    nv::modeset::PresentationLocks locks(device);
    Event cursorStarted;
    Event cursorDone;
    std::thread cursor;
    {
        auto lifetime = locks.DeviceShared();
        auto flip = locks.Programming();
        cursor = std::thread([&] {
            auto cursorLifetime = locks.DeviceShared();
            cursorStarted.Signal();
            auto programming = locks.Programming();
            cursorDone.Signal();
        });
        Check(cursorStarted.Wait());
        Check(!cursorDone.Wait(50));
    }
    Check(cursorDone.Wait());
    cursor.join();
}

void TestProducerExclusion(bool copy)
{
    SRWLOCK device = SRWLOCK_INIT;
    nv::modeset::PresentationLocks locks(device);
    Event producerStarted;
    Event producerDone;
    Event cursorDone;
    std::thread producer;
    {
        auto lifetime = locks.DeviceShared();
        auto owner = copy ? locks.Copy() : locks.Present();
        auto movedOwner = std::move(owner);
        producer = std::thread([&] {
            auto producerLifetime = locks.DeviceShared();
            producerStarted.Signal();
            auto second = copy ? locks.Copy() : locks.Present();
            producerDone.Signal();
        });
        Check(producerStarted.Wait());
        Check(!producerDone.Wait(50));
        std::thread cursor([&] {
            auto cursorLifetime = locks.DeviceShared();
            auto programming = locks.Programming();
            cursorDone.Signal();
        });
        Check(cursorDone.Wait());
        cursor.join();
    }
    Check(producerDone.Wait());
    producer.join();
}

void TestStress()
{
    SRWLOCK device = SRWLOCK_INIT;
    nv::modeset::PresentationLocks locks(device);
    std::atomic<unsigned> activeCopy{0};
    std::atomic<unsigned> activeProgramming{0};
    std::atomic<unsigned> copies{0};
    std::atomic<unsigned> programs{0};
    std::array<std::thread, 6> workers;
    for (unsigned index = 0; index < workers.size(); ++index)
    {
        workers[index] = std::thread([&, index] {
            for (unsigned iteration = 0; iteration < 2000; ++iteration)
            {
                auto lifetime = locks.DeviceShared();
                if (index % 2 == 0)
                {
                    auto copy = locks.Copy();
                    Check(activeCopy.fetch_add(1) == 0);
                    std::this_thread::yield();
                    Check(activeCopy.fetch_sub(1) == 1);
                    ++copies;
                }
                else
                {
                    auto programming = locks.Programming();
                    Check(activeProgramming.fetch_add(1) == 0);
                    std::this_thread::yield();
                    Check(activeProgramming.fetch_sub(1) == 1);
                    ++programs;
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    Check(copies == 6000 && programs == 6000);
    Check(activeCopy == 0 && activeProgramming == 0);
}

} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<nv::modeset::PresentationLocks>);
    TestCopyAndCursor();
    TestProgrammingExclusion();
    TestProducerExclusion(true);
    TestProducerExclusion(false);
    TestStress();
    std::printf("PASS: %u production presentation lock checks; cursor/copy overlap, "
        "flip exclusion, CE/presenter serialization, and lifecycle teardown\n", cases.load());
}
