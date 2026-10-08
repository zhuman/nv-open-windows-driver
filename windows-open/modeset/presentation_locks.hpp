#pragma once

#include <windows.h>
#include <utility>

namespace nv::modeset {

class PresentationLocks final
{
    template<bool Shared>
    class Guard final
    {
    public:
        explicit Guard(SRWLOCK& lock) noexcept : lock_(&lock)
        {
            if (Shared)
                AcquireSRWLockShared(lock_);
            else
                AcquireSRWLockExclusive(lock_);
        }
        ~Guard() noexcept
        {
            if (lock_ != nullptr)
            {
                if (Shared)
                    ReleaseSRWLockShared(lock_);
                else
                    ReleaseSRWLockExclusive(lock_);
            }
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : lock_(std::exchange(other.lock_, nullptr)) {}
        Guard& operator=(Guard&&) = delete;

    private:
        SRWLOCK* lock_;
    };

public:
    explicit PresentationLocks(SRWLOCK& deviceLock) noexcept : deviceLock_(deviceLock) {}
    PresentationLocks(const PresentationLocks&) = delete;
    PresentationLocks& operator=(const PresentationLocks&) = delete;

    auto DeviceShared() noexcept { return Guard<true>(deviceLock_); }
    auto DeviceExclusive() noexcept { return Guard<false>(deviceLock_); }
    auto Copy() noexcept { return Guard<false>(copyLock_); }
    auto Programming() noexcept { return Guard<false>(programmingLock_); }
    auto Present() noexcept { return Guard<false>(presentLock_); }

private:
    // Device first; programming may acquire copy for LUTs, never the reverse.
    SRWLOCK& deviceLock_;
    SRWLOCK copyLock_ = SRWLOCK_INIT;
    SRWLOCK programmingLock_ = SRWLOCK_INIT;
    SRWLOCK presentLock_ = SRWLOCK_INIT;
};

} // namespace nv::modeset
