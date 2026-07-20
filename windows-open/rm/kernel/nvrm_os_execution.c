#include "nvplatform_sync.h"

NvBool osIsRaisedIRQL(void)
{
    return NvPlatformIsRaisedExecutionLevel();
}

NvBool osIsISR(void)
{
    return NvPlatformIsInterruptExecutionLevel();
}

void osSpinLoop(void)
{
    NvPlatformProcessorPause();
}

NV_STATUS osFlushCpuCache(void)
{
    return NvPlatformFlushCpuCaches() ? NV_OK : NV_ERR_NOT_SUPPORTED;
}

void osFlushCpuWriteCombineBuffer(void)
{
    NvPlatformFlushWriteBuffer();
}