#include <gtest/gtest.h>

#include "nvplatform_common.h"
#include "nvplatform_system_rm.h"
#include "common_services.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

unsigned Calls = 0;
NvU32 LastLevel = 0;
char LastMessage[1024]{};
std::atomic<unsigned> ConcurrentCalls{0};


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

class DebugLog : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NvPlatformSetDebugLog(nullptr);
        Calls = 0;
        LastLevel = 0;
        std::memset(LastMessage, 0, sizeof(LastMessage));
        ConcurrentCalls.store(0);
    }
    void TearDown() override { NvPlatformSetDebugLog(nullptr); }
};

TEST_F(DebugLog, RegistrationFormattingAndNullMessages)
{
    NvPlatformDebugPrintf(1, "Unregistered");
    EXPECT_TRUE(Calls == 0);
    NvPlatformSetDebugLog(Log);
    NvPlatformDebugPrintf(7, "value=%u %s", 42U, "100%");
    EXPECT_TRUE(Calls == 1 && LastLevel == 7);
    EXPECT_TRUE(std::strcmp(LastMessage, "value=42 100%") == 0);
    NvPlatformDebugPrintf(7, nullptr);
    EXPECT_TRUE(Calls == 1);

    NvPlatformSetDebugLog(nullptr);
    const unsigned before = Calls;
    NvPlatformDebugPrintf(1, "Unregistered again");
    EXPECT_TRUE(Calls == before);
}

TEST_F(DebugLog, TruncatesAtMessageCapacity)
{
    NvPlatformSetDebugLog(Log);
    char longMessage[2048];
    std::memset(longMessage, 'a', sizeof(longMessage) - 1);
    longMessage[sizeof(longMessage) - 1] = '\0';
    NvPlatformDebugPrintf(8, "%s", longMessage);
    EXPECT_TRUE(Calls == 1 && LastLevel == 8);
    EXPECT_TRUE(std::strlen(LastMessage) == 1023 && LastMessage[1022] == 'a');

}

TEST_F(DebugLog, CommonAndPlatformServicesForwardMessages)
{
    // Forwarded text is already formatted; literal percent signs must survive.
    NvPlatformSetDebugLog(Log);
    auto* services = nv::iddcx::GetCommonHostServices();
    ASSERT_NE(services, nullptr);
    services->log(services->providerContext, 9, "common 100%");
    EXPECT_TRUE(Calls == 1 && LastLevel == 9);
    EXPECT_TRUE(std::strcmp(LastMessage, "common 100%") == 0);
    auto* platform = NvPlatformGetCommonHostServices();
    ASSERT_NE(platform, nullptr);
    platform->log(platform->providerContext, 10, "platform");
    EXPECT_TRUE(Calls == 2 && LastLevel == 10);
    platform->log(platform->providerContext, 10, nullptr);
    EXPECT_TRUE(Calls == 2);
}

TEST_F(DebugLog, FormatsXidAndCapturesAssertionFrames)
{
    NvPlatformSetDebugLog(Log);
    Xid(56, "channel=%u 100%%", 7U);
    EXPECT_TRUE(Calls == 1);
    EXPECT_TRUE(std::strcmp(LastMessage, "NVRM: Xid 56: channel=7 100%") == 0);
    Xid(56, nullptr);
    EXPECT_TRUE(Calls == 1);
    // Stack depth depends on the build: bound it without requiring fixed frames.
    NvPlatformRmCaptureAssertion();
    EXPECT_TRUE(Calls > 2 && Calls <= 18);
    EXPECT_TRUE(std::strstr(LastMessage, "NVRM: assertion frame ") == LastMessage);

}

TEST_F(DebugLog, ConcurrentLoggingAndRegistrationRemainSafe)
{
    // Null-registration windows may drop messages. Only the uninterrupted
    // first batch guarantees an exact callback count.
    NvPlatformSetDebugLog(ConcurrentLog);
    std::thread writer([] {
        for (unsigned index = 0; index < 100; ++index)
            NvPlatformDebugPrintf(index, "Concurrent message");
    });
    writer.join();
    EXPECT_TRUE(ConcurrentCalls.load() == 100);
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
    EXPECT_TRUE(ConcurrentCalls.load() >= 100 && ConcurrentCalls.load() <= 200);
}
