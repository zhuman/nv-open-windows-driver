#include "nvplatform_common.h"
#include "nvplatform_system_rm.h"
#include "common_services.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

unsigned Checks = 0;
unsigned Calls = 0;
NvU32 LastLevel = 0;
char LastMessage[1024]{};
std::atomic<unsigned> ConcurrentCalls{0};

void Check(bool value)
{
    ++Checks;
    if (!value)
    {
        std::fprintf(stderr, "Debug log check %u failed\n", Checks);
        std::exit(1);
    }
}

void Log(NvU32 level, const char* message)
{
    ++Calls;
    LastLevel = level;
    std::strcpy(LastMessage, message);
}

void ConcurrentLog(NvU32, const char*)
{
    ConcurrentCalls.fetch_add(1, std::memory_order_relaxed);
}

void Xid(NvU32 xid, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    NvPlatformRmLogErrorV(xid, format, args);
    va_end(args);
}

} // namespace

int main()
{
    NvPlatformDebugPrintf(1, "Unregistered");
    Check(Calls == 0);
    NvPlatformSetDebugLog(Log);
    NvPlatformDebugPrintf(7, "value=%u %s", 42U, "100%");
    Check(Calls == 1 && LastLevel == 7);
    Check(std::strcmp(LastMessage, "value=42 100%") == 0);
    NvPlatformDebugPrintf(7, nullptr);
    Check(Calls == 1);

    char longMessage[2048];
    std::memset(longMessage, 'a', sizeof(longMessage) - 1);
    longMessage[sizeof(longMessage) - 1] = '\0';
    NvPlatformDebugPrintf(8, "%s", longMessage);
    Check(Calls == 2 && LastLevel == 8);
    Check(std::strlen(LastMessage) == 1023 && LastMessage[1022] == 'a');

    auto* services = nv::iddcx::GetCommonHostServices();
    services->log(services->providerContext, 9, "common 100%");
    Check(Calls == 3 && LastLevel == 9);
    Check(std::strcmp(LastMessage, "common 100%") == 0);
    auto* platform = NvPlatformGetCommonHostServices();
    platform->log(platform->providerContext, 10, "platform");
    Check(Calls == 4 && LastLevel == 10);
    platform->log(platform->providerContext, 10, nullptr);
    Check(Calls == 4);

    Xid(56, "channel=%u 100%%", 7U);
    Check(Calls == 5);
    Check(std::strcmp(LastMessage, "NVRM: Xid 56: channel=7 100%") == 0);
    Xid(56, nullptr);
    Check(Calls == 5);
    NvPlatformRmCaptureAssertion();
    Check(Calls > 6 && Calls <= 22);
    Check(std::strstr(LastMessage, "NVRM: assertion frame ") == LastMessage);

    NvPlatformSetDebugLog(nullptr);
    const unsigned before = Calls;
    NvPlatformDebugPrintf(1, "Unregistered again");
    Check(Calls == before);

    NvPlatformSetDebugLog(ConcurrentLog);
    std::thread writer([] {
        for (unsigned index = 0; index < 100; ++index)
            NvPlatformDebugPrintf(index, "Concurrent message");
    });
    writer.join();
    Check(ConcurrentCalls.load() == 100);
    std::thread toggler([] {
        for (unsigned index = 0; index < 100; ++index)
        {
            NvPlatformSetDebugLog(nullptr);
            NvPlatformSetDebugLog(ConcurrentLog);
        }
    });
    std::thread logger([] {
        for (unsigned index = 0; index < 100; ++index)
            NvPlatformDebugPrintf(index, "Concurrent registration");
    });
    toggler.join();
    logger.join();
    NvPlatformSetDebugLog(nullptr);
    Check(ConcurrentCalls.load() >= 100 && ConcurrentCalls.load() <= 200);
    std::printf("%u production debug log checks passed\n", Checks);
}
