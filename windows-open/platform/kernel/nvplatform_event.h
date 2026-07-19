#pragma once

#include "nvstatus.h"
#include "nvtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

NV_STATUS NvPlatformEventReferenceUserHandle(
    NvU64 eventHandle,
    void** eventObject);
void NvPlatformEventReference(void* eventObject);
void NvPlatformEventDereference(void* eventObject);
void NvPlatformEventSignal(void* eventObject);

#ifdef __cplusplus
}
#endif