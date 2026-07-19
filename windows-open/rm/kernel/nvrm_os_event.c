#include "nvplatform_event.h"

#include "g_event_api_nvoc.h"
#include "g_os_nvoc.h"

NvU32 gpumgrGetSubDeviceInstanceFromGpu(OBJGPU *pGpu);

static NV_STATUS
nvrmSignalOsEvent
(
    PEVENTNOTIFICATION pNotifyEvent,
    NvBool bDoRefCount
)
{
    void *pEventObject = NvP64_VALUE(pNotifyEvent->Data);

    if (pEventObject == NULL)
    {
        return NV_ERR_INVALID_EVENT;
    }

    if (bDoRefCount)
    {
        NvPlatformEventReference(pEventObject);
    }

    NvPlatformEventSignal(pEventObject);

    if (bDoRefCount)
    {
        NvPlatformEventDereference(pEventObject);
    }

    return NV_OK;
}

static NV_STATUS
nvrmNotifyOne
(
    PEVENTNOTIFICATION pNotifyEvent,
    void *pEventData,
    NvU32 data,
    NV_STATUS status,
    NvBool bDoRefCount
)
{
    switch (pNotifyEvent->NotifyType)
    {
        case NV01_EVENT_OS_EVENT:
            return nvrmSignalOsEvent(pNotifyEvent, bDoRefCount);

        case NV01_EVENT_KERNEL_CALLBACK:
        {
            NVOS10_EVENT_KERNEL_CALLBACK *pCallback =
                NvP64_VALUE(pNotifyEvent->Data);

            if ((pCallback == NULL) || (pCallback->func == NULL))
            {
                return NV_ERR_INVALID_EVENT;
            }

            pCallback->func(pCallback->arg);
            return NV_OK;
        }

        case NV01_EVENT_KERNEL_CALLBACK_EX:
        {
            NVOS10_EVENT_KERNEL_CALLBACK_EX *pCallback =
                NvP64_VALUE(pNotifyEvent->Data);

            if ((pCallback == NULL) || (pCallback->func == NULL))
            {
                return NV_ERR_INVALID_EVENT;
            }

            pCallback->func(
                pCallback->arg,
                pEventData,
                pNotifyEvent->hEvent,
                data,
                status);
            return NV_OK;
        }

        default:
            return NV_ERR_NOT_SUPPORTED;
    }
}

NV_STATUS
osUserHandleToKernelPtr
(
    NvU32 hClient,
    NvP64 hEvent,
    NvP64 *pEvent
)
{
    void *pEventObject;
    NV_STATUS status;

    (void)hClient;

    if (pEvent == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = NvPlatformEventReferenceUserHandle(
        (NvU64)hEvent,
        &pEventObject);
    if (status != NV_OK)
    {
        return status;
    }

    *pEvent = NV_PTR_TO_NvP64(pEventObject);
    return NV_OK;
}

NV_STATUS osReferenceObjectCount(void *pEvent)
{
    if (pEvent == NULL)
    {
        return NV_ERR_INVALID_EVENT;
    }

    NvPlatformEventReference(pEvent);
    return NV_OK;
}

NV_STATUS osDereferenceObjectCount(void *pEvent)
{
    if (pEvent == NULL)
    {
        return NV_ERR_INVALID_EVENT;
    }

    NvPlatformEventDereference(pEvent);
    return NV_OK;
}

NvU32 osSetEvent(OBJGPU *pGpu, NvP64 eventId)
{
    EVENTNOTIFICATION notifyEvent = {0};

    (void)pGpu;

    notifyEvent.NotifyType = NV01_EVENT_OS_EVENT;
    notifyEvent.Data = eventId;
    return nvrmSignalOsEvent(&notifyEvent, NV_TRUE) == NV_OK;
}

NV_STATUS
osNotifyEvent
(
    OBJGPU *pGpu,
    PEVENTNOTIFICATION pNotifyEvent,
    NvU32 method,
    NvU32 data,
    NV_STATUS status,
    NvBool bDoRefCount
)
{
    (void)pGpu;
    (void)method;

    if (pNotifyEvent == NULL)
    {
        return NV_ERR_INVALID_EVENT;
    }

    return nvrmNotifyOne(
        pNotifyEvent,
        NULL,
        data,
        status,
        bDoRefCount);
}

NV_STATUS
osEventNotification
(
    OBJGPU *pGpu,
    PEVENTNOTIFICATION pNotifyEvent,
    NvU32 notifyIndex,
    void *pEventData,
    NvU32 eventDataSize
)
{
    return osEventNotificationWithInfo(
        pGpu,
        pNotifyEvent,
        notifyIndex,
        0,
        0,
        pEventData,
        eventDataSize);
}

NV_STATUS
osEventNotificationWithInfo
(
    OBJGPU *pGpu,
    PEVENTNOTIFICATION pNotifyEvent,
    NvU32 notifyIndex,
    NvU32 info32,
    NvU16 info16,
    void *pEventData,
    NvU32 eventDataSize
)
{
    NvBool bMatchSubdevice = NV_FALSE;
    NV_STATUS result = NV_OK;

    (void)info16;
    (void)info32;
    (void)eventDataSize;

    if ((notifyIndex != OS_EVENT_NOTIFICATION_INDEX_ALL) &&
        ((notifyIndex & OS_EVENT_NOTIFICATION_INDEX_MATCH_SUBDEV) != 0))
    {
        bMatchSubdevice = NV_TRUE;
        notifyIndex &= ~OS_EVENT_NOTIFICATION_INDEX_MATCH_SUBDEV;
    }

    for (; pNotifyEvent != NULL; pNotifyEvent = pNotifyEvent->Next)
    {
        NV_STATUS status;

        if ((notifyIndex != OS_EVENT_NOTIFICATION_INDEX_ALL) &&
            (pNotifyEvent->NotifyIndex != notifyIndex))
        {
            continue;
        }

        if (bMatchSubdevice &&
            ((pGpu == NULL) ||
             (gpumgrGetSubDeviceInstanceFromGpu(pGpu) !=
              pNotifyEvent->subdeviceInst)))
        {
            continue;
        }

        status = nvrmNotifyOne(
            pNotifyEvent,
            pEventData,
            0,
            NV_OK,
            NV_TRUE);
        if ((status != NV_OK) && (result == NV_OK))
        {
            result = status;
        }
    }

    return result;
}

NV_STATUS
osObjectEventNotification
(
    NvHandle hClient,
    NvHandle hObject,
    NvU32 hClass,
    PEVENTNOTIFICATION pNotifyEvent,
    NvU32 notifyIndex,
    void *pEventData,
    NvU32 eventDataSize
)
{
    (void)hClient;
    (void)hObject;
    (void)hClass;

    return osEventNotification(
        NULL,
        pNotifyEvent,
        notifyIndex,
        pEventData,
        eventDataSize);
}