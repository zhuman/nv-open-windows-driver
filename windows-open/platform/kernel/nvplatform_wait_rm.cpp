#include <ntddk.h>

#include "nvplatform_wait_rm.h"

namespace {

constexpr ULONG WaitQueuePoolTag =
    static_cast<ULONG>('N') |
    (static_cast<ULONG>('v') << 8) |
    (static_cast<ULONG>('W') << 16) |
    (static_cast<ULONG>('q') << 24);

struct WaitQueue
{
    KEVENT event;
};

} // namespace

extern "C" NV_STATUS NvPlatformRmWaitQueueCreate(void** waitQueue)
{
    if (waitQueue == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *waitQueue = nullptr;
    auto* queue = static_cast<WaitQueue*>(ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(WaitQueue),
        WaitQueuePoolTag));
    if (queue == nullptr)
    {
        return NV_ERR_NO_MEMORY;
    }

    KeInitializeEvent(&queue->event, SynchronizationEvent, FALSE);
    *waitQueue = queue;
    return NV_OK;
}

extern "C" void NvPlatformRmWaitQueueDestroy(void* waitQueue)
{
    if (waitQueue != nullptr)
    {
        ExFreePoolWithTag(waitQueue, WaitQueuePoolTag);
    }
}

extern "C" void NvPlatformRmWaitQueueWait(void* waitQueue, NvBool alertable)
{
    auto* queue = static_cast<WaitQueue*>(waitQueue);
    if (queue == nullptr || KeGetCurrentIrql() > APC_LEVEL)
    {
        return;
    }

    (void)KeWaitForSingleObject(
        &queue->event,
        Executive,
        KernelMode,
        alertable == NV_TRUE,
        nullptr);
}

extern "C" void NvPlatformRmWaitQueueWake(void* waitQueue)
{
    auto* queue = static_cast<WaitQueue*>(waitQueue);
    if (queue != nullptr)
    {
        KeSetEvent(&queue->event, IO_NO_INCREMENT, FALSE);
    }
}