#include <gtest/gtest.h>

#include "../../modeset/presentation_locks.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>

namespace {

class Event
{
public:
    Event() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle)
    {
        EXPECT_TRUE(handle_ != nullptr);
    }
    void Signal() { EXPECT_TRUE(SetEvent(handle_.get()) != FALSE); }
    bool Wait(DWORD timeout = 5000)
    {
        const DWORD result = WaitForSingleObject(handle_.get(), timeout);
        EXPECT_TRUE(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT);
        return result == WAIT_OBJECT_0;
    }
private:
    std::unique_ptr<void, decltype(&CloseHandle)> handle_;
};

// Start handshakes avoid probing a never-started worker; bounded waits check
// exclusion, not scheduler fairness or a latency guarantee.
TEST(PresentationLocks, CopyOverlapsCursorButBlocksShutdown)
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
        EXPECT_TRUE(cursorDone.Wait());
        cursor.join();

        shutdown = std::thread([&] {
            shutdownStarted.Signal();
            auto exclusive = locks.DeviceExclusive();
            shutdownDone.Signal();
        });
        EXPECT_TRUE(shutdownStarted.Wait());
        EXPECT_TRUE(!shutdownDone.Wait(50));
    }
    EXPECT_TRUE(shutdownDone.Wait());
    shutdown.join();
}

TEST(PresentationLocks, ProgrammingExcludesOtherProgramming)
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
        EXPECT_TRUE(cursorStarted.Wait());
        EXPECT_TRUE(!cursorDone.Wait(50));
    }
    EXPECT_TRUE(cursorDone.Wait());
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
        EXPECT_TRUE(producerStarted.Wait());
        EXPECT_TRUE(!producerDone.Wait(50));
        std::thread cursor([&] {
            auto cursorLifetime = locks.DeviceShared();
            auto programming = locks.Programming();
            cursorDone.Signal();
        });
        EXPECT_TRUE(cursorDone.Wait());
        cursor.join();
    }
    EXPECT_TRUE(producerDone.Wait());
    producer.join();
}

TEST(PresentationLocks, SerializesSixConcurrentWorkers)
{
    // Copy and programming may overlap; each class must still serialize itself.
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
                    EXPECT_TRUE(activeCopy.fetch_add(1) == 0);
                    std::this_thread::yield();
                    EXPECT_TRUE(activeCopy.fetch_sub(1) == 1);
                    ++copies;
                }
                else
                {
                    auto programming = locks.Programming();
                    EXPECT_TRUE(activeProgramming.fetch_add(1) == 0);
                    std::this_thread::yield();
                    EXPECT_TRUE(activeProgramming.fetch_sub(1) == 1);
                    ++programs;
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    EXPECT_TRUE(copies == 6000 && programs == 6000);
    EXPECT_TRUE(activeCopy == 0 && activeProgramming == 0);
}

} // namespace

static_assert(!std::is_copy_constructible_v<nv::modeset::PresentationLocks>);

TEST(PresentationLocks, CopyProducersExcludeEachOther) { TestProducerExclusion(true); }
TEST(PresentationLocks, PresentersExcludeEachOther) { TestProducerExclusion(false); }
