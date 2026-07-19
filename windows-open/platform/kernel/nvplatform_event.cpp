#include "nvplatform_event.h"

#include <ntddk.h>

NV_STATUS NvPlatformEventReferenceUserHandle(
    NvU64 eventHandle,
    void** eventObject)
{
    if (eventHandle == 0 || eventObject == nullptr)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *eventObject = nullptr;
    const NTSTATUS status = ObReferenceObjectByHandle(
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(eventHandle)),
        EVENT_MODIFY_STATE,
        *ExEventObjectType,
        UserMode,
        eventObject,
        nullptr);
    if (NT_SUCCESS(status))
    {
        return NV_OK;
    }

    if (status == STATUS_INVALID_HANDLE || status == STATUS_OBJECT_TYPE_MISMATCH)
    {
        return NV_ERR_INVALID_EVENT;
    }

    if (status == STATUS_ACCESS_DENIED)
    {
        return NV_ERR_INSUFFICIENT_PERMISSIONS;
    }

    return NV_ERR_OPERATING_SYSTEM;
}

void NvPlatformEventReference(void* eventObject)
{
    ObReferenceObject(eventObject);
}

void NvPlatformEventDereference(void* eventObject)
{
    ObDereferenceObject(eventObject);
}

void NvPlatformEventSignal(void* eventObject)
{
    KeSetEvent(static_cast<PKEVENT>(eventObject), IO_NO_INCREMENT, FALSE);
}