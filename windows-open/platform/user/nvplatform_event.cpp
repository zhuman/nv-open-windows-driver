#include "nvplatform_event.h"

#include <windows.h>

namespace {

struct EventReference
{
    volatile LONG referenceCount;
    HANDLE handle;
};

} // namespace

extern "C" NV_STATUS NvPlatformEventReferenceUserHandle(
    NvU64 eventHandle,
    void** eventObject)
{
    if (eventHandle == 0 || eventObject == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *eventObject = nullptr;

    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(eventHandle)),
            GetCurrentProcess(),
            &duplicate,
            EVENT_MODIFY_STATE,
            FALSE,
            0))
    {
        return GetLastError() == ERROR_ACCESS_DENIED
            ? NV_ERR_INSUFFICIENT_PERMISSIONS
            : NV_ERR_INVALID_EVENT;
    }

    auto* reference = static_cast<EventReference*>(
        HeapAlloc(GetProcessHeap(), 0, sizeof(EventReference)));
    if (reference == nullptr)
    {
        CloseHandle(duplicate);
        return NV_ERR_NO_MEMORY;
    }
    reference->referenceCount = 1;
    reference->handle = duplicate;
    *eventObject = reference;
    return NV_OK;
}

extern "C" void NvPlatformEventReference(void* eventObject)
{
    if (eventObject != nullptr)
    {
        InterlockedIncrement(
            &static_cast<EventReference*>(eventObject)->referenceCount);
    }
}

extern "C" void NvPlatformEventDereference(void* eventObject)
{
    if (eventObject == nullptr)
    {
        return;
    }
    auto* reference = static_cast<EventReference*>(eventObject);
    if (InterlockedDecrement(&reference->referenceCount) == 0)
    {
        CloseHandle(reference->handle);
        HeapFree(GetProcessHeap(), 0, reference);
    }
}

extern "C" void NvPlatformEventSignal(void* eventObject)
{
    if (eventObject != nullptr)
    {
        SetEvent(static_cast<EventReference*>(eventObject)->handle);
    }
}