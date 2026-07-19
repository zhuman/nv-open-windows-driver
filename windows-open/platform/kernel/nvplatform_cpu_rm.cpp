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

#if defined(_M_AMD64) || defined(_M_IX86)
    int registers[4]{};
    __cpuidex(registers, operation, suboperation);
    *eax = static_cast<NvU32>(registers[0]);
    *ebx = static_cast<NvU32>(registers[1]);
    *ecx = static_cast<NvU32>(registers[2]);
    *edx = static_cast<NvU32>(registers[3]);
    return 1;
#else
    UNREFERENCED_PARAMETER(operation);
    UNREFERENCED_PARAMETER(suboperation);
    *eax = 0;
    *ebx = 0;
    *ecx = 0;
    *edx = 0;
    return 0;
#endif
}

extern "C" NvU32 NvPlatformGetRmCpuFrequencyMhz(void)
{
#if defined(_M_AMD64) || defined(_M_IX86)
    int registers[4]{};
    __cpuidex(registers, 0, 0);
    if (registers[0] < 0x16)
    {
        return 0;
    }

    __cpuidex(registers, 0x16, 0);
    return static_cast<NvU32>(registers[0]) & 0xffffU;
#else
    return 0;
#endif
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

extern "C" NvU32 NvPlatformRmReadCr4(void)
{
#if defined(_M_AMD64) || defined(_M_IX86)
    return static_cast<NvU32>(__readcr4());
#else
    return 0;
#endif
}

extern "C" NvU64 NvPlatformRmReadXcr0(void)
{
#if defined(_M_AMD64) || defined(_M_IX86)
    return static_cast<NvU64>(_xgetbv(0));
#else
    return 0;
#endif
}