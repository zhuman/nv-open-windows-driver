#include "nvplatform_process_rm.h"

NV_STATUS osAttachToProcess(void** processState, NvU32 processId)
{
    return NvPlatformRmAttachToProcess(processState, processId);
}

void osDetachFromProcess(void* processState)
{
    NvPlatformRmDetachFromProcess(processState);
}

NV_STATUS osIsKernelBuffer(void* address, NvU32 size)
{
    return NvPlatformRmValidateKernelBuffer(address, size);
}