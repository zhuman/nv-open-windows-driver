#include "g_os_nvoc.h"

NvBool osCheckCallback(OBJGPU* gpu)
{
    (void)gpu;
    return NV_FALSE;
}

NvBool osCheckCallback_v2(OBJGPU* gpu)
{
    (void)gpu;
    return NV_FALSE;
}

RC_CALLBACK_STATUS osRCCallback(
    OBJGPU* gpu,
    NvHandle client,
    NvHandle device,
    NvHandle fifo,
    NvHandle channel,
    NvU32 errorLevel,
    NvU32 errorType,
    NvU32* data,
    void* reenablePusher)
{
    (void)gpu;
    (void)client;
    (void)device;
    (void)fifo;
    (void)channel;
    (void)errorLevel;
    (void)errorType;
    (void)data;
    (void)reenablePusher;
    return RC_CALLBACK_IGNORE;
}

RC_CALLBACK_STATUS osRCCallback_v2(
    OBJGPU* gpu,
    NvHandle client,
    NvHandle device,
    NvHandle fifo,
    NvHandle channel,
    NvU32 errorLevel,
    NvU32 errorType,
    NvBool deferRcRequested,
    NvU32* data,
    void* reenablePusher)
{
    (void)gpu;
    (void)client;
    (void)device;
    (void)fifo;
    (void)channel;
    (void)errorLevel;
    (void)errorType;
    (void)deferRcRequested;
    (void)data;
    (void)reenablePusher;
    return RC_CALLBACK_IGNORE;
}
