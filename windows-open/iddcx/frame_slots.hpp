#pragma once

#include <windows.h>
#include <array>

namespace nv::iddcx {

class FrameSlots final
{
public:
    enum class State { Free, Copying, Ready, Uploading, Queued, Front };
    HRESULT Configure(UINT count, UINT front) noexcept
    {
        if (count < 2 || count > slots_.size() || front >= count)
            return E_INVALIDARG;
        count_ = count;
        slots_.fill(State::Free);
        slots_[front] = State::Front;
        latest_ = Invalid;
        return S_OK;
    }
    UINT Acquire() noexcept
    {
        for (UINT index = 0; index < count_; ++index)
            if (slots_[index] == State::Free)
            {
                slots_[index] = State::Copying;
                return index;
            }
        return Invalid;
    }
    HRESULT Ready(UINT index) noexcept { return Transition(index, State::Copying, State::Ready); }
    HRESULT Upload(UINT index) noexcept { return Transition(index, State::Ready, State::Uploading); }
    HRESULT Queue(UINT index) noexcept
    {
        if (Queued() >= 2)
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        const HRESULT result = Transition(index, State::Uploading, State::Queued);
        if (SUCCEEDED(result))
            latest_ = index;
        return result;
    }
    HRESULT Latch(UINT latest) noexcept
    {
        if (latest >= count_ || latest != latest_ || slots_[latest] != State::Queued)
            return E_INVALIDARG;
        for (UINT index = 0; index < count_; ++index)
            if (slots_[index] == State::Queued || slots_[index] == State::Front)
                slots_[index] = State::Free;
        slots_[latest] = State::Front;
        latest_ = Invalid;
        return S_OK;
    }
    UINT Queued() const noexcept
    {
        UINT count = 0;
        for (UINT index = 0; index < count_; ++index)
            count += slots_[index] == State::Queued;
        return count;
    }
    UINT Latest() const noexcept { return latest_; }
    State Get(UINT index) const noexcept { return slots_[index]; }
    static constexpr UINT Invalid = ~0U;

private:
    HRESULT Transition(UINT index, State from, State to) noexcept
    {
        if (index >= count_ || slots_[index] != from)
            return E_INVALIDARG;
        slots_[index] = to;
        return S_OK;
    }
    std::array<State, 4> slots_ = {};
    UINT count_ = 0;
    UINT latest_ = Invalid;
};

class PlaneBufferSlots final
{
public:
    UINT Acquire(bool reuse = false) noexcept
    {
        if (queued_ != FrameSlots::Invalid || prepared_ != FrameSlots::Invalid)
            return FrameSlots::Invalid;
        if (reuse)
            return prepared_ = front_;
        for (UINT index = 0; index < 3; ++index)
            if (index != front_)
                return prepared_ = index;
        return FrameSlots::Invalid;
    }
    HRESULT Queue() noexcept
    {
        if (prepared_ == FrameSlots::Invalid || queued_ != FrameSlots::Invalid)
            return E_UNEXPECTED;
        queued_ = prepared_;
        prepared_ = FrameSlots::Invalid;
        return S_OK;
    }
    HRESULT Latch() noexcept
    {
        if (queued_ == FrameSlots::Invalid) return E_UNEXPECTED;
        front_ = queued_;
        queued_ = FrameSlots::Invalid;
        return S_OK;
    }
    UINT Front() const noexcept { return front_; }
    UINT Queued() const noexcept { return queued_; }

private:
    UINT front_ = FrameSlots::Invalid;
    UINT queued_ = FrameSlots::Invalid;
    UINT prepared_ = FrameSlots::Invalid;
};

} // namespace nv::iddcx

namespace nv::iddcx {

class FrameRamSlots final
{
public:
    enum class State { Free, Copying, Ready, Reading };
    HRESULT Configure(UINT count) noexcept
    {
        if (count < 2 || count > states_.size())
            return E_INVALIDARG;
        count_ = count;
        states_.fill(State::Free);
        return S_OK;
    }
    UINT Acquire() noexcept
    {
        for (UINT index = 0; index < count_; ++index)
            if (states_[index] == State::Free)
            {
                states_[index] = State::Copying;
                return index;
            }
        return FrameSlots::Invalid;
    }
    HRESULT Ready(UINT index) noexcept { return Change(index, State::Copying, State::Ready); }
    HRESULT Read(UINT index) noexcept { return Change(index, State::Ready, State::Reading); }
    HRESULT Release(UINT index) noexcept { return Change(index, State::Reading, State::Free); }
    State Get(UINT index) const noexcept { return states_[index]; }
private:
    HRESULT Change(UINT index, State expected, State next) noexcept
    {
        if (index >= count_ || states_[index] != expected)
            return E_INVALIDARG;
        states_[index] = next;
        return S_OK;
    }
    std::array<State, 4> states_ = {};
    UINT count_ = 0;
};

} // namespace nv::iddcx
