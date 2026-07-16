#include "nvport_windows.h"

#include "nvport/nvport.h"

#include <intrin.h>

NV_STATUS portReadMsr(NvU32 address, NvU64* value)
{
    (void)address;

    if (value == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *value = 0;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portWriteMsr(NvU32 address, NvU64 value)
{
    (void)address;
    (void)value;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuGetLogicalTopology(PORT_CPU_LOGICAL_TOPOLOGY* cpuTopology)
{
    if (cpuTopology == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    portMemSet(cpuTopology, 0, sizeof(*cpuTopology));
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExCpuId(
    NvU32* cpuInfo,
    NvU32 functionId,
    NvU32 subfunctionId)
{
    int registers[4];

    if (cpuInfo == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    __cpuidex(registers, (int)functionId, (int)subfunctionId);
    cpuInfo[0] = (NvU32)registers[0];
    cpuInfo[1] = (NvU32)registers[1];
    cpuInfo[2] = (NvU32)registers[2];
    cpuInfo[3] = (NvU32)registers[3];
    return NV_OK;
}

NvS64 portCpuExQueryPerformanceCounter(NvS64* frequency)
{
    if (frequency != NULL)
    {
        *frequency = 10000000;
    }

    return (NvS64)nvPortWindowsGetMonotonicTime100ns();
}

void portCpuExEnablePmc(void)
{
}

NV_STATUS portCpuExReadPmc(NvU32 address, NvU64* value)
{
    (void)address;

    if (value == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *value = 0;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExAllocImcBarDesc(PORT_CPU_BAR_DESC* imcBarDesc)
{
    if (imcBarDesc == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    portMemSet(imcBarDesc, 0, sizeof(*imcBarDesc));
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExFreeImcBarDesc(PORT_CPU_BAR_DESC* imcBarDesc)
{
    if (imcBarDesc == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    portMemSet(imcBarDesc, 0, sizeof(*imcBarDesc));
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExResetPmu(void)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExProgramPmu(NvU32 numValidEvents, NvU32* perfEvents)
{
    (void)numValidEvents;
    (void)perfEvents;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExGetDRamReads(NvU64* numReads)
{
    if (numReads == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *numReads = 0;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExGetDRamWrites(NvU64* numWrites)
{
    if (numWrites == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *numWrites = 0;
    return NV_ERR_NOT_SUPPORTED;
}

NvBool portCpuExIsMsrSupported(NvU32 address)
{
    (void)address;
    return NV_FALSE;
}

NvBool portCpuExIsDramRwCountingSupported(void)
{
    return NV_FALSE;
}

NV_STATUS portCpuExAcquireHardwareCounters(NvP64* resourceHandle)
{
    if (resourceHandle == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *resourceHandle = 0;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS portCpuExReleaseHardwareCounters(NvP64 resourceHandle)
{
    (void)resourceHandle;
    return NV_ERR_NOT_SUPPORTED;
}