#pragma once

extern "C" {
#include <ntddk.h>

NTSTATUS NvPlatformSystemInitialize(_In_ PDRIVER_OBJECT driverObject) noexcept;
void NvPlatformSystemShutdown(void) noexcept;
}