#pragma once

#if !PORT_IS_KERNEL_BUILD
#error "This file can only be compiled as part of the kernel build."
#endif

void nvPortWindowsDebugPrintString(const char* string, NvLength length);

#define portDbgInitialize()
#define portDbgShutdown()

PORT_DEBUG_INLINE void portDbgPrintString(
    const char* string,
    NvLength length)
{
    nvPortWindowsDebugPrintString(string, length);
}

#define PORT_BREAKPOINT() __debugbreak()