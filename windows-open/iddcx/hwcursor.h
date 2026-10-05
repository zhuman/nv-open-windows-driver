#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <wil/resource.h>
#include <memory>

namespace nv::iddcx {

class Output;

class HwCursor final
{
public:
    explicit HwCursor(Output& output) noexcept;
    ~HwCursor() noexcept;
    NTSTATUS Setup() noexcept;
    void Pause() noexcept;
    void Resume() noexcept;

    HwCursor(const HwCursor&) = delete;
    HwCursor& operator=(const HwCursor&) = delete;

private:
    static DWORD WINAPI ThreadEntry(void* context) noexcept;
    void Process() noexcept;

    Output* output_;
    wil::srwlock lock_;
    wil::unique_handle changedEvent_;
    wil::unique_handle stopEvent_;
    wil::unique_handle thread_;
    std::unique_ptr<BYTE[]> shapeBuffer_;
    bool paused_;
    bool rebind_;
};

} // namespace nv::iddcx