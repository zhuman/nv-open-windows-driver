#include "nvplatform_cpu_rm.h"

extern "C" {
#include <ntddk.h>
}

#include <intrin.h>

extern "C" int NvPlatformRmCpuId(
    int operation,
    int suboperation,
    NvU32* eax,
    NvU32* ebx,
    NvU32* ecx,
    NvU32* edx)
{
    if (eax == nullptr || ebx == nullptr || ecx == nullptr || edx == nullptr)
    {
        return 0;
    }

    int registers[4]{};
    __cpuidex(registers, operation, suboperation);
    *eax = static_cast<NvU32>(registers[0]);
    *ebx = static_cast<NvU32>(registers[1]);
    *ecx = static_cast<NvU32>(registers[2]);
    *edx = static_cast<NvU32>(registers[3]);
    return 1;
}

extern "C" NvU32 NvPlatformGetRmCpuFrequencyMhz(void)
{
    int registers[4]{};
    __cpuidex(registers, 0, 0);
    if (registers[0] < 0x16)
    {
        return 0;
    }

    __cpuidex(registers, 0x16, 0);
    return static_cast<NvU32>(registers[0]) & 0xffffU;
}

extern "C" NvU32 NvPlatformGetRmCpuCount(void)
{
    return static_cast<NvU32>(KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS));
}

extern "C" NvU32 NvPlatformGetRmCurrentProcessorNumber(void)
{
    PROCESSOR_NUMBER processor{};
    KeGetCurrentProcessorNumberEx(&processor);

    NvU32 number = processor.Number;
    for (USHORT group = 0; group < processor.Group; ++group)
    {
        number += static_cast<NvU32>(KeQueryMaximumProcessorCountEx(group));
    }

    return number;
}