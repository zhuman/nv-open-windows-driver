#pragma once

#if PORT_IS_KERNEL_BUILD
#error "This file can only be compiled as part of the user-mode build."
#endif

void nvPortWindowsDebugPrintString(const char* string, NvLength length);
void NvPlatformDebugPrintf(NvU32 level, const char* format, ...);

#define portDbgInitialize()
#define portDbgShutdown()

PORT_DEBUG_INLINE void portDbgPrintString(
    const char* string,
    NvLength length)
{
    nvPortWindowsDebugPrintString(string, length);
}

#define portDbgPrintf(format, ...) \
    NvPlatformDebugPrintf(0, format, ##__VA_ARGS__)
#undef portDbgPrintf_SUPPORTED
#define portDbgPrintf_SUPPORTED 1

#define portDbgExPrintfLevel(level, format, ...) \
    NvPlatformDebugPrintf(level, format, ##__VA_ARGS__)
#undef portDbgExPrintfLevel_SUPPORTED
#define portDbgExPrintfLevel_SUPPORTED 1

#define PORT_BREAKPOINT() __debugbreak()