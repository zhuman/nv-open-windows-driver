#include "nvplatform_cpu_rm.h"

int osNv_cpuid(
    int operation,
    int suboperation,
    NvU32* eax,
    NvU32* ebx,
    NvU32* ecx,
    NvU32* edx)
{
    return NvPlatformRmCpuId(operation, suboperation, eax, ebx, ecx, edx);
}

NvU32 osGetCpuFrequency(void)
{
    return NvPlatformGetRmCpuFrequencyMhz();
}

NvU32 osGetCpuCount(void)
{
    return NvPlatformGetRmCpuCount();
}

NvU32 osGetCurrentProcessorNumber(void)
{
    return NvPlatformGetRmCurrentProcessorNumber();
}