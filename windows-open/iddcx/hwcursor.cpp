#include "hwcursor.h"
#include "output.hpp"
#include "trace.h"

#include <cstring>
#include <new>
#include <utility>

#include "hwcursor.tmh"

namespace nv::iddcx {

HwCursor::HwCursor(Output& output) noexcept
    : output_(&output), paused_(true), rebind_(true)
{
}

HwCursor::~HwCursor() noexcept
{
    if (stopEvent_)
    {
        SetEvent(stopEvent_.get());
    }
    if (thread_)
    {
        WaitForSingleObject(thread_.get(), INFINITE);
    }
}

NTSTATUS HwCursor::Setup() noexcept
{
    auto lock = lock_.lock_exclusive();
    const UINT size = output_->DisplayState().maxCursorSize;
    if (!thread_)
    {
        shapeBuffer_.reset(new (std::nothrow) BYTE[size * size * 4]);
        changedEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!shapeBuffer_ || !changedEvent_ || !stopEvent_)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        thread_.reset(CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr));
        if (!thread_)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    IDARG_IN_SETUP_HWCURSOR input = {};
    input.CursorInfo.Size = sizeof(input.CursorInfo);
    input.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_NONE;
    input.CursorInfo.AlphaCursorSupport = TRUE;
    input.CursorInfo.MaxX = input.CursorInfo.MaxY = size;
    input.hNewCursorDataAvailable = changedEvent_.get();
    const NTSTATUS status = IddCxMonitorSetupHardwareCursor(output_->Monitor(), &input);
    TraceEvents(
        NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_WARNING,
        TRACE_DRIVER,
        "Hardware cursor setup: %!STATUS! size=%u monitor=%p",
        status, size, output_->Monitor());
    if (NT_SUCCESS(status))
    {
        paused_ = false;
        rebind_ = true;
        SetEvent(changedEvent_.get());
    }
    return status;
}

void HwCursor::Pause() noexcept
{
    auto lock = lock_.lock_exclusive();
    paused_ = true;
}

void HwCursor::Resume() noexcept
{
    auto lock = lock_.lock_exclusive();
    paused_ = false;
    rebind_ = true;
    if (changedEvent_)
    {
        SetEvent(changedEvent_.get());
    }
}

DWORD WINAPI HwCursor::ThreadEntry(void* context) noexcept
{
    static_cast<HwCursor*>(context)->Process();
    return 0;
}

void HwCursor::Process() noexcept
{
    const HANDLE handles[] = { stopEvent_.get(), changedEvent_.get() };
    const UINT size = output_->DisplayState().maxCursorSize;
    unique_nvkms_surface image;
    unique_nvkms_surface pending;
    DWORD shapeId = 0;
    bool visible = false;
    INT lastX = 0;
    INT lastY = 0;
    INT positionX = 0;
    INT positionY = 0;
    UINT positionId = 0;
    UINT lastPositionId = 0;
    UINT lastWhiteLevel = 80;
    bool positionKnown = false;
    bool retry = false;
    ULONG updates = 0;
    for (;;)
    {
        const DWORD wait = WaitForMultipleObjects(
            ARRAYSIZE(handles), handles, FALSE, retry ? 16 : INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
        {
            break;
        }
        retry = false;
        auto cursorLock = lock_.lock_exclusive();
        if (paused_)
        {
            continue;
        }
        auto outputLock = output_->LockFrameStateShared();
        if (!output_->Active())
        {
            continue;
        }
        IDARG_IN_QUERY_HWCURSOR input = {};
        input.LastShapeId = rebind_ ? 0 : shapeId;
        input.ShapeBufferSizeInBytes = size * size * 4;
        input.pShapeBuffer = shapeBuffer_.get();
        IDARG_OUT_QUERY_HWCURSOR3 cursor = {};
        const HRESULT query = IddCxMonitorQueryHardwareCursor3(
            output_->Monitor(), &input, &cursor);
        if (FAILED(query))
        {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "Hardware cursor query: 0x%08X", query);
            continue;
        }
        if (cursor.PositionValid)
        {
            positionX = cursor.X;
            positionY = cursor.Y;
            positionId = cursor.PositionId;
            positionKnown = true;
        }
        if (cursor.IsCursorShapeUpdated && cursor.IsCursorVisible)
        {
            const auto& shape = cursor.CursorShapeInfo;
            if (shape.CursorType != IDDCX_CURSOR_SHAPE_TYPE_ALPHA ||
                shape.Width == 0 || shape.Height == 0 ||
                shape.Width > size || shape.Height > size ||
                shape.Pitch < shape.Width * 4 ||
                static_cast<UINT64>(shape.Pitch) * shape.Height > input.ShapeBufferSizeInBytes)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Hardware cursor invalid shape: type=%u width=%u height=%u pitch=%u",
                    shape.CursorType, shape.Width, shape.Height, shape.Pitch);
                continue;
            }
            NV_KMS_WINDOWS_SURFACE* surface = nullptr;
            void* pixels = nullptr;
            NvU32 pitch = 0;
            const NV_STATUS allocation = NvKmsWindowsCreateScanoutSurface(
                size, size, NV_KMS_WINDOWS_SCANOUT_FORMAT_ARGB8,
                &surface, &pixels, &pitch);
            if (allocation != NV_OK)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Hardware cursor allocation: 0x%08X", allocation);
                continue;
            }
            pending.reset(surface);
            std::memset(pixels, 0, static_cast<size_t>(pitch) * size);
            for (UINT row = 0; row < shape.Height; ++row)
            {
                std::memcpy(static_cast<BYTE*>(pixels) + row * pitch,
                    shapeBuffer_.get() + row * shape.Pitch, shape.Width * 4);
            }
            shapeId = shape.ShapeId;
        }
        const bool show = cursor.IsCursorVisible && (pending || image) &&
            positionKnown && positionX >= -32768 && positionX <= 32767 &&
            positionY >= -32768 && positionY <= 32767;
        const UINT whiteLevel =
            output_->ColorSpace() == NV_KMS_WINDOWS_COLOR_SPACE_HDR10 &&
            cursor.SdrWhiteLevel != 0 ? cursor.SdrWhiteLevel : 80;
        const INT x = show ? positionX : 0;
        const INT y = show ? positionY : 0;
        const bool imageChanged = rebind_ || show != visible || (show && pending) ||
            whiteLevel != lastWhiteLevel;
        if (!imageChanged && x == lastX && y == lastY &&
            positionId == lastPositionId)
        {
            continue;
        }
        const NV_STATUS status = NvKmsWindowsUpdateCursor(
            output_->AssignedHead(), output_->DisplayState().displayHandle,
            show ? (pending ? pending.get() : image.get()) : nullptr,
            imageChanged ? NV_TRUE : NV_FALSE, x, y, whiteLevel);
        if (status != NV_OK)
        {
            retry = status == NV_ERR_IN_USE;
            if (!retry)
            {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "Hardware cursor update: 0x%08X head=%u", status, output_->AssignedHead());
            }
            continue;
        }
        if (show && pending)
        {
            image = std::move(pending);
        }
        visible = show;
        lastX = x;
        lastY = y;
        lastPositionId = positionId;
        lastWhiteLevel = whiteLevel;
        rebind_ = false;
        ++updates;
        TraceEvents(updates == 1 || imageChanged ?
            TRACE_LEVEL_INFORMATION : TRACE_LEVEL_VERBOSE,
            TRACE_DRIVER,
            "Hardware cursor update: head=%u shape=%u image=%u visible=%u x=%d y=%d updates=%u white=%u",
            output_->AssignedHead(), shapeId, imageChanged, visible, x, y,
            updates, whiteLevel);
    }
    auto outputLock = output_->LockFrameStateShared();
    if (output_->Active())
    {
        NvKmsWindowsUpdateCursor(output_->AssignedHead(),
            output_->DisplayState().displayHandle, nullptr, NV_TRUE, 0, 0, 80);
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "Hardware cursor worker stopped: updates=%u", updates);
}

} // namespace nv::iddcx