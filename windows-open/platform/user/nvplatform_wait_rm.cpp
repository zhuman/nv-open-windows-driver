#include "nvplatform_wait_rm.h"

#include <windows.h>

extern "C" NV_STATUS NvPlatformRmWaitQueueCreate(void** waitQueue)
{
    if (waitQueue == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *waitQueue = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return *waitQueue != nullptr ? NV_OK : NV_ERR_NO_MEMORY;
}

extern "C" void NvPlatformRmWaitQueueDestroy(void* waitQueue)
{
    if (waitQueue != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(waitQueue));
    }
}

extern "C" void NvPlatformRmWaitQueueWait(void* waitQueue, NvBool alertable)
{
    if (waitQueue != nullptr)
    {
        WaitForSingleObjectEx(
            static_cast<HANDLE>(waitQueue),
            INFINITE,
            alertable == NV_TRUE);
    }
}

extern "C" void NvPlatformRmWaitQueueWake(void* waitQueue)
{
    if (waitQueue != nullptr)
    {
        SetEvent(static_cast<HANDLE>(waitQueue));
    }
}