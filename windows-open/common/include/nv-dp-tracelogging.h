#pragma once

#include "nvtypes.h"

#define NV_DP_TRACE_TEXT       0x1ULL
#define NV_DP_TRACE_EVENTS     0x2ULL
#define NV_DP_TRACE_AUX        0x4ULL
#define NV_DP_TRACE_RM_AUX     0x8ULL

#ifdef __cplusplus
extern "C" {
#endif

typedef NvU32 (*NV_DP_TRACE_RM_CONTROL)(
    NvU32 client, NvU32 object, NvU32 command, void *params, NvU32 paramsSize);

NvU32 NvDpTraceInitialize(void);
void NvDpTraceShutdown(void);
void NvDpTraceText(NvU32 level, const char *gpuPrefix, const char *message);
NvBool NvDpTraceEventEnabled(NvU32 priority);
void NvDpTraceEvent(NvU32 event, NvU32 priority, NvU32 count, const NvU64 *args);
NvU32 NvDpTraceControl(
    NvU32 client, NvU32 object, NvU32 command, void *params, NvU32 paramsSize,
    NV_DP_TRACE_RM_CONTROL control);

#ifdef __cplusplus
}
#endif
