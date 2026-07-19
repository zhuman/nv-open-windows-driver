#include "nvplatform_system_rm.h"

extern "C" {
#include <ntddk.h>
#include <ntpoapi.h>
}

extern "C" NvBool NvPlatformRmIsNotebook(void)
{
    SYSTEM_POWER_CAPABILITIES capabilities{};
    const NTSTATUS status = ZwPowerInformation(
        SystemPowerCapabilities,
        nullptr,
        0,
        &capabilities,
        sizeof(capabilities));

    return NT_SUCCESS(status) && capabilities.SystemBatteriesPresent
        ? NV_TRUE
        : NV_FALSE;
}