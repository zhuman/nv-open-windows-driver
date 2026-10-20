#include <gtest/gtest.h>
#include <windows.h>
#include <evntrace.h>
#include <tdh.h>
#include <winmeta.h>

#include "nv-dp-tracelogging.h"
#include "dp_hostimp.h"
#include "nvkms-rmapi.h"
#include "nvkms-utils.h"
#include "nv-kernel-rmapi-ops.h"
#include "nvstatus.h"
#include <ctrl/ctrl0073/ctrl0073dp.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr GUID ProviderGuid =
    {0xf4b9d5d8, 0x9a13, 0x4b5f, {0xa2, 0x95, 0x5e, 0x61, 0x6c, 0x83, 0xc6, 0xc1}};

struct Event
{
    std::wstring name;
    UCHAR level = 0;
    std::map<std::wstring, std::vector<BYTE>> fields;

    NvU64 Number(const wchar_t* field) const
    {
        const auto found = fields.find(field);
        if (found == fields.end() || found->second.size() > sizeof(NvU64))
        {
            ADD_FAILURE() << "Missing or oversized numeric property";
            return 0;
        }
        NvU64 result = 0;
        std::memcpy(&result, found->second.data(), found->second.size());
        return result;
    }

    std::string Text(const wchar_t* field) const
    {
        const auto found = fields.find(field);
        if (found == fields.end() || found->second.empty())
        {
            ADD_FAILURE() << "Missing text property";
            return {};
        }
        return std::string(reinterpret_cast<const char*>(found->second.data()));
    }
};

NvU32 originalCalls = 0;
NvU32 recorderCalls = 0;
NvU32 operationStatus = NV_OK;
NvU32 replyType = NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK;
NvU32 replySize = 3;
NvU32 recorderStatus = NV_OK;
NvU32 recorderCount = 0;
NvU32 recorderLength = 3;
NvU32 lastClient = 0;
NvU32 lastObject = 0;
NvU32 lastSubdevice = 0;
NV0073_CTRL_DP_AUXCH_CTRL_PARAMS lastRequest = {};

NvU32 Control(NvU32 client, NvU32 object, NvU32 command, void* params, NvU32)
{
    lastClient = client;
    lastObject = object;
    if (command == NV0073_CTRL_CMD_DP_GET_AUXLOGGER_BUFFER_DATA)
    {
        ++recorderCalls;
        auto& data = *static_cast<NV0073_CTRL_CMD_DP_GET_AUXLOGGER_BUFFER_DATA_PARAMS*>(params);
        lastSubdevice = data.subDeviceInstance;
        EXPECT_EQ(MAX_LOGS_PER_POLL, data.dpAuxBufferReadSize);
        data.dpNumMessagesRead = recorderCount;
        for (NvU32 i = 0; i < (std::min)(recorderCount, MAX_LOGS_PER_POLL); ++i)
        {
            auto& packet = data.dpAuxBuffer[i];
            packet.auxCount = i;
            packet.auxEvents = NV_DP_AUXLOGGER_REQUEST_TYPE_MOTREQWSTAT |
                (NV_DP_AUXLOGGER_REPLY_TYPE_DEFER << 4);
            packet.auxOutPort = 4;
            packet.auxPortAddress = 0x102;
            packet.auxRequestTimeStamp = 500;
            packet.auxReplyTimeStamp = 501;
            packet.auxMessageReqSize = 3;
            packet.auxMessageReplySize = recorderLength;
            packet.auxPacket[0] = 0xaa;
            packet.auxPacket[1] = 0xbb;
            packet.auxPacket[2] = 0xcc;
        }
        return recorderStatus;
    }
    ++originalCalls;
    if (command == NV0073_CTRL_CMD_DP_AUXCH_CTRL)
    {
        auto& data = *static_cast<NV0073_CTRL_DP_AUXCH_CTRL_PARAMS*>(params);
        lastRequest = data;
        data.replyType = replyType;
        data.size = replySize;
        data.retryTimeMs = 17;
        data.data[0] = 0x12;
        data.data[1] = 0x34;
        data.data[2] = 0x56;
        for (NvU32 i = 3; i < NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE; ++i)
            data.data[i] = static_cast<NvU8>(0x80 + i);
    }
    return operationStatus;
}

} // namespace

extern "C" NvBool nvDoDebugLogging(void) { return NV_TRUE; }
extern "C" void* nvInternalAlloc(size_t size, NvBool zero)
{
    return zero ? std::calloc(1, size) : std::malloc(size);
}
extern "C" void nvInternalFree(void* pointer) { std::free(pointer); }
#if defined(DEBUG)
extern "C" void* nvDebugAlloc(size_t size, int, const char*) { return std::malloc(size); }
extern "C" void nvDebugFree(void* pointer) { std::free(pointer); }
extern "C" void nvDebugAssert(const char*, const char*, const char*, unsigned)
{
    ADD_FAILURE() << "Unexpected production assertion";
}
#endif
extern "C" void nvVEvoLog(NVEvoLogType, NvU8, const char*, va_list)
{
    ADD_FAILURE() << "Unexpected legacy text log in structured/AUX test";
}

extern "C" void nvkms_call_rm(void* raw)
{
    auto& ops = *static_cast<nvidia_kernel_rmapi_ops_t*>(raw);
    ASSERT_EQ(NV04_CONTROL, ops.op);
    ops.params.control.status = Control(ops.params.control.hClient,
        ops.params.control.hObject, ops.params.control.cmd,
        NvP64_VALUE(ops.params.control.params), ops.params.control.paramsSize);
}

class DpTrace : public ::testing::Test
{
protected:
    struct Properties
    {
        EVENT_TRACE_PROPERTIES value;
        wchar_t name[128];
        wchar_t file[MAX_PATH];
    } properties = {};
    TRACEHANDLE session = 0;
    std::vector<Event> events;

    void SetUp() override
    {
        originalCalls = recorderCalls = 0;
        operationStatus = recorderStatus = NV_OK;
        replyType = NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK;
        replySize = recorderLength = 3;
        recorderCount = 0;
        ASSERT_EQ(ERROR_SUCCESS, NvDpTraceInitialize());

        wchar_t temp[MAX_PATH];
        ASSERT_NE(0U, GetTempPathW(MAX_PATH, temp));
        swprintf_s(properties.name, L"NvDpTest-%u", GetCurrentProcessId());
        swprintf_s(properties.file, L"%sNvDpTest-%u.etl", temp, GetCurrentProcessId());
        properties.value.Wnode.BufferSize = sizeof(properties);
        properties.value.Wnode.Guid = ProviderGuid;
        properties.value.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties.value.Wnode.ClientContext = 1;
        properties.value.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL |
            EVENT_TRACE_PRIVATE_LOGGER_MODE | EVENT_TRACE_PRIVATE_IN_PROC;
        properties.value.BufferSize = 64;
        properties.value.MinimumBuffers = 2;
        properties.value.MaximumBuffers = 8;
        properties.value.LoggerNameOffset = offsetof(Properties, name);
        properties.value.LogFileNameOffset = offsetof(Properties, file);
        ASSERT_EQ(ERROR_SUCCESS,
            StartTraceW(&session, properties.name, &properties.value));
    }

    void Enable(NvU64 keywords, UCHAR level = WINEVENT_LEVEL_VERBOSE)
    {
        ASSERT_EQ(ERROR_SUCCESS, EnableTraceEx2(session, &ProviderGuid,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, keywords, 0, 0, nullptr));
    }

    static void WINAPI Record(EVENT_RECORD* record)
    {
        if (record->EventHeader.ProviderId != ProviderGuid)
            return;
        auto& self = *static_cast<DpTrace*>(record->UserContext);
        ULONG size = 0;
        ASSERT_EQ(ERROR_INSUFFICIENT_BUFFER,
            TdhGetEventInformation(record, 0, nullptr, nullptr, &size));
        std::vector<BYTE> buffer(size);
        auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data());
        ASSERT_EQ(ERROR_SUCCESS, TdhGetEventInformation(record, 0, nullptr, info, &size));
        ASSERT_NE(0U, info->EventNameOffset);
        Event event;
        event.name = reinterpret_cast<wchar_t*>(buffer.data() + info->EventNameOffset);
        event.level = record->EventHeader.EventDescriptor.Level;
        for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i)
        {
            const auto* name = reinterpret_cast<const wchar_t*>(
                buffer.data() + info->EventPropertyInfoArray[i].NameOffset);
            PROPERTY_DATA_DESCRIPTOR descriptor = {};
            descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
            descriptor.ArrayIndex = ULONG_MAX;
            ULONG length = 0;
            ASSERT_EQ(ERROR_SUCCESS,
                TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &length));
            auto& bytes = event.fields[name];
            bytes.resize(length);
            if (length != 0)
                ASSERT_EQ(ERROR_SUCCESS, TdhGetProperty(record, 0, nullptr,
                    1, &descriptor, length, bytes.data()));
        }
        self.events.push_back(std::move(event));
    }

    void Read()
    {
        ASSERT_NE(0ULL, session);
        ASSERT_EQ(ERROR_SUCCESS,
            ControlTraceW(session, properties.name, &properties.value, EVENT_TRACE_CONTROL_STOP));
        session = 0;
        EXPECT_EQ(0U, properties.value.EventsLost);
        EVENT_TRACE_LOGFILEW logfile = {};
        logfile.LogFileName = properties.file;
        logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
        logfile.EventRecordCallback = Record;
        logfile.Context = this;
        TRACEHANDLE consumer = OpenTraceW(&logfile);
        ASSERT_NE(INVALID_PROCESSTRACE_HANDLE, consumer);
        EXPECT_EQ(ERROR_SUCCESS, ProcessTrace(&consumer, 1, nullptr, nullptr));
        EXPECT_EQ(ERROR_SUCCESS, CloseTrace(consumer));
    }

    void TearDown() override
    {
        if (session != 0)
            EXPECT_EQ(ERROR_SUCCESS, ControlTraceW(session, properties.name,
                &properties.value, EVENT_TRACE_CONTROL_STOP));
        NvDpTraceShutdown();
        if (properties.file[0] != L'\0' && GetFileAttributesW(properties.file) != INVALID_FILE_ATTRIBUTES)
            EXPECT_TRUE(DeleteFileW(properties.file));
    }

    std::vector<Event> Named(const wchar_t* name) const
    {
        std::vector<Event> result;
        for (const auto& event : events)
            if (event.name == name)
                result.push_back(event);
        return result;
    }

    NV0073_CTRL_DP_AUXCH_CTRL_PARAMS Aux(NvU32 cmd = 9, NvU32 size = 2)
    {
        NV0073_CTRL_DP_AUXCH_CTRL_PARAMS data = {};
        data.subDeviceInstance = 2;
        data.displayId = 0x100;
        data.cmd = cmd;
        data.addr = 0x202;
        data.size = size;
        data.data[0] = 0xab;
        data.data[1] = 0xcd;
        data.data[2] = 0xef;
        return data;
    }

    NvU32 Submit(NV0073_CTRL_DP_AUXCH_CTRL_PARAMS& data)
    {
        return nvRmApiControl(11, 22, NV0073_CTRL_CMD_DP_AUXCH_CTRL, &data, sizeof(data));
    }
};

TEST_F(DpTrace, DisabledCapturePreservesForwardingWithoutRecorderCalls)
{
    auto data = Aux();
    operationStatus = NV_ERR_TIMEOUT;
    EXPECT_EQ(operationStatus, Submit(data));
    EXPECT_EQ(1U, originalCalls);
    EXPECT_EQ(0U, recorderCalls);
    EXPECT_EQ(11U, lastClient);
    EXPECT_EQ(22U, lastObject);
    EXPECT_EQ(0x202U, lastRequest.addr);
    EXPECT_EQ(replySize, data.size);
    Read();
    EXPECT_TRUE(events.empty());
}

TEST_F(DpTrace, RuntimeKeywordsAndLevelIndependentlyGatePayloadCapture)
{
    Enable(NV_DP_TRACE_TEXT | NV_DP_TRACE_EVENTS);
    auto data = Aux();
    Submit(data);
    NvDpTraceText(0, "", "before");
    Enable(NV_DP_TRACE_AUX, WINEVENT_LEVEL_INFO);
    data = Aux();
    Submit(data);
    Enable(NV_DP_TRACE_AUX);
    data = Aux();
    Submit(data);
    Enable(NV_DP_TRACE_TEXT);
    data = Aux();
    Submit(data);
    EXPECT_EQ(0U, recorderCalls);
    Read();
    EXPECT_EQ(1U, Named(L"AuxRequest").size());
    EXPECT_EQ(1U, Named(L"AuxResponse").size());
    EXPECT_EQ(1U, Named(L"DpText").size());
}

TEST_F(DpTrace, TextPreservesSeverityPercentSignsAndLongMessages)
{
    Enable(NV_DP_TRACE_TEXT, WINEVENT_LEVEL_WARNING);
    NvDpTraceText(0, "", "filtered");
    NvDpTraceText(1, "GPU:2: ", "100% literal");
    const std::string message(11000, 'x');
    NvDpTraceText(2, "", message.c_str());
    Read();
    const auto text = Named(L"DpText");
    ASSERT_EQ(4U, text.size());
    EXPECT_EQ(WINEVENT_LEVEL_WARNING, text[0].level);
    EXPECT_EQ("100% literal", text[0].Text(L"Message"));
    EXPECT_EQ("GPU:2: ", text[0].Text(L"GpuPrefix"));
    std::string joined;
    for (unsigned i = 1; i < text.size(); ++i)
    {
        EXPECT_EQ(WINEVENT_LEVEL_ERROR, text[i].level);
        EXPECT_EQ("Driver", text[i].Text(L"Origin"));
        EXPECT_EQ(i - 1, text[i].Number(L"ChunkIndex"));
        EXPECT_EQ(text[1].Number(L"MessageId"), text[i].Number(L"MessageId"));
        EXPECT_EQ(i + 1 == text.size(), text[i].Number(L"LastChunk") != 0);
        joined += text[i].Text(L"Message");
    }
    EXPECT_EQ(message, joined);
}

TEST_F(DpTrace, StructuredEventsConsumeNormalizedVariadicWidthsAndValidateCounts)
{
    Enable(NV_DP_TRACE_EVENTS);
    NV_DPTRACE_INFO(NOTIFY_ATTACH_BEGIN, 2U, 0x123456789abcdef0ULL, 7680, 4320,
        30, true, false);
    NV_DPTRACE_INFO(LINK_TRAINING_START, true, 20000000000ULL, 4, 2, 2, true, 2);
    NV_DPTRACE_INFO(NEW_MST_DEVICE, 1, 2, 3, 4, 5, true, false, 3);
    NV_DPTRACE_WARNING(MESSAGE_EXPIRED, 0x20, 1, 2, 3, 4, 5);
    NV_DPTRACE_INFO(NOTIFY_ATTACH_END);
    NvDpTraceEvent(999, TRACE_DP_PRIORITY_INFO, 0, nullptr);
    NvDpTraceEvent(TRACE_DP_ID_HOTPLUG, TRACE_DP_PRIORITY_INFO, 1, nullptr);
    Read();
    const auto attach = Named(L"AttachBegin");
    ASSERT_EQ(1U, attach.size());
    EXPECT_EQ(0x123456789abcdef0ULL, attach[0].Number(L"PixelClockHz"));
    EXPECT_EQ(30U, attach[0].Number(L"Depth"));
    EXPECT_EQ(1U, attach[0].Number(L"Dsc"));
    const auto training = Named(L"LinkTrainingStart");
    ASSERT_EQ(1U, training.size());
    EXPECT_EQ(20000000000ULL, training[0].Number(L"PeakRate"));
    EXPECT_EQ(2U, training[0].Number(L"Target"));
    const auto expired = Named(L"MessageExpired");
    ASSERT_EQ(1U, expired.size());
    EXPECT_EQ(WINEVENT_LEVEL_WARNING, expired[0].level);
    EXPECT_EQ(0x20U, expired[0].Number(L"RequestId"));
    EXPECT_EQ(5U, expired[0].Number(L"Address3"));
    EXPECT_EQ(1U, Named(L"NewMstDevice").size());
    EXPECT_EQ(1U, Named(L"AttachEnd").size());
    EXPECT_EQ(2U, Named(L"InvalidDpEvent").size());
}

TEST_F(DpTrace, AuxReadsAndWritesCarryExactBytesAndCorrelation)
{
    Enable(NV_DP_TRACE_AUX);
    auto write = Aux(8);
    Submit(write);
    auto read = Aux();
    Submit(read);
    Read();
    const auto requests = Named(L"AuxRequest");
    const auto replies = Named(L"AuxResponse");
    ASSERT_EQ(2U, requests.size());
    ASSERT_EQ(2U, replies.size());
    EXPECT_EQ((std::vector<BYTE>{0xab, 0xcd, 0xef}), requests[0].fields.at(L"Payload"));
    EXPECT_TRUE(replies[0].fields.at(L"Payload").empty());
    EXPECT_TRUE(requests[1].fields.at(L"Payload").empty());
    EXPECT_EQ((std::vector<BYTE>{0x12, 0x34, 0x56}), replies[1].fields.at(L"Payload"));
    for (unsigned i = 0; i < 2; ++i)
    {
        EXPECT_EQ(requests[i].Number(L"TransactionId"), replies[i].Number(L"TransactionId"));
        EXPECT_GE(replies[i].Number(L"Qpc"), requests[i].Number(L"Qpc"));
        EXPECT_EQ("Driver", requests[i].Text(L"Origin"));
        EXPECT_EQ(0x100U, requests[i].Number(L"DisplayId"));
        EXPECT_EQ(2U, requests[i].Number(L"Subdevice"));
    }
}

TEST_F(DpTrace, EveryExistingStructuredEventHasAValidatedNamedSchema)
{
    Enable(NV_DP_TRACE_EVENTS);
    NV_DPTRACE_INFO(HOTPLUG, true, true, false);
    NV_DPTRACE_INFO(NEW_SINK_DETECTED, 1, 2, 3, 4, 5, true, 0x1234, 0x5678);
    NV_DPTRACE_INFO(NEW_SINK_REPORTED, 1, 2, 3, 4, 5);
    NV_DPTRACE_INFO(NEW_MST_DEVICE, 1, 2, 3, 4, 5, true, false, 3);
    NV_DPTRACE_WARNING(LOST_DEVICE, 1, 2, 3, 4, 5);
    NV_DPTRACE_INFO(LINK_ASSESSMENT, 8100000000ULL, 4);
    NV_DPTRACE_INFO(LINK_TRAINING_START, true, 8100000000ULL, 4, 0, 0, false, 0);
    NV_DPTRACE_INFO(LINK_TRAINING_DONE, NV_OK, 8100000000ULL, 4);
    NV_DPTRACE_INFO(NOTIFY_ATTACH_BEGIN, 2, 594000000ULL, 3840, 2160, 24, true, true);
    NV_DPTRACE_INFO(NOTIFY_ATTACH_BEGIN_STATUS, true);
    NV_DPTRACE_INFO(NOTIFY_ATTACH_END);
    NV_DPTRACE_INFO(NOTIFY_DETACH_BEGIN);
    NV_DPTRACE_INFO(NOTIFY_DETACH_END);
    NV_DPTRACE_WARNING(MESSAGE_EXPIRED, 0x20, 1, 2, 3, 4, 5);
    Read();
    EXPECT_EQ(14U, events.size());
    EXPECT_TRUE(Named(L"InvalidDpEvent").empty());
    EXPECT_EQ(1U, Named(L"Hotplug").size());
    EXPECT_EQ(1U, Named(L"NewSinkDetected").size());
    EXPECT_EQ(1U, Named(L"NewSinkReported").size());
    EXPECT_EQ(1U, Named(L"NewMstDevice").size());
    EXPECT_EQ(1U, Named(L"LostDevice").size());
    EXPECT_EQ(1U, Named(L"LinkAssessment").size());
    EXPECT_EQ(1U, Named(L"LinkTrainingStart").size());
    EXPECT_EQ(1U, Named(L"LinkTrainingDone").size());
    EXPECT_EQ(1U, Named(L"AttachBegin").size());
    EXPECT_EQ(1U, Named(L"AttachStatus").size());
    EXPECT_EQ(1U, Named(L"AttachEnd").size());
    EXPECT_EQ(1U, Named(L"DetachBegin").size());
    EXPECT_EQ(1U, Named(L"DetachEnd").size());
    EXPECT_EQ(1U, Named(L"MessageExpired").size());
}

TEST_F(DpTrace, LinkControlsIncludeDp2TrainingPhasesAndCanPollTheRecorder)
{
    Enable(NV_DP_TRACE_EVENTS | NV_DP_TRACE_RM_AUX);
    NV0073_CTRL_DP_CTRL_PARAMS lt = {};
    lt.subDeviceInstance = 1;
    lt.displayId = 0x40;
    lt.cmd = 0x1234;
    lt.data = 0x5678;
    nvRmApiControl(11, 22, NV0073_CTRL_CMD_DP_CTRL, &lt, sizeof(lt));
    NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL_PARAMS lt2 = {};
    lt2.subDeviceInstance = 1;
    lt2.displayId = 0x80;
    lt2.cmd = 0x4567;
    lt2.data = 0x89ab;
    operationStatus = NV_ERR_TIMEOUT;
    EXPECT_EQ(operationStatus,
        nvRmApiControl(11, 22, NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL, &lt2, sizeof(lt2)));
    EXPECT_EQ(2U, originalCalls);
    EXPECT_EQ(4U, recorderCalls);
    Read();
    const auto requests = Named(L"LinkControlRequest");
    const auto replies = Named(L"LinkControlResponse");
    ASSERT_EQ(2U, requests.size());
    ASSERT_EQ(2U, replies.size());
    EXPECT_EQ(NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL, requests[1].Number(L"Control"));
    EXPECT_EQ(0x4567U, requests[1].Number(L"Command"));
    EXPECT_EQ(0x89abU, requests[1].Number(L"Data"));
    EXPECT_EQ(operationStatus, replies[1].Number(L"Status"));
    EXPECT_EQ(requests[1].Number(L"TransactionId"), replies[1].Number(L"TransactionId"));
}

TEST_F(DpTrace, ShortReadsWriteStatusAndOversizedRepliesRetainRawLengths)
{
    Enable(NV_DP_TRACE_AUX);
    auto data = Aux(9, 15);
    replySize = 3;
    Submit(data);
    data = Aux(6, 0);
    Submit(data);
    data = Aux(9, 0);
    replySize = 16;
    Submit(data);
    Read();
    const auto requests = Named(L"AuxRequest");
    const auto replies = Named(L"AuxResponse");
    ASSERT_EQ(3U, requests.size());
    ASSERT_EQ(3U, replies.size());
    EXPECT_EQ(16U, requests[0].Number(L"RequestedLength"));
    EXPECT_EQ(3U, replies[0].Number(L"ReportedLength"));
    EXPECT_EQ((std::vector<BYTE>{0x12, 0x34, 0x56}), replies[0].fields.at(L"Payload"));
    EXPECT_TRUE(requests[1].fields.at(L"Payload").empty());
    EXPECT_TRUE(replies[1].fields.at(L"Payload").empty());
    EXPECT_EQ(0U, requests[1].Number(L"RequestedLength"));
    EXPECT_EQ(16U, replies[2].Number(L"ReportedLength"));
    EXPECT_EQ(0U, replies[2].Number(L"ValidLength"));
    EXPECT_TRUE(replies[2].fields.at(L"Payload").empty());
}

TEST_F(DpTrace, CapturesAllSixteenBytesAndZeroLengthAckWithoutPadding)
{
    Enable(NV_DP_TRACE_AUX);
    auto write = Aux(8, 15);
    std::vector<BYTE> expectedWrite(16);
    for (NvU32 i = 0; i < 16; ++i)
        write.data[i] = expectedWrite[i] = static_cast<NvU8>(0xa0 + i);
    Submit(write);
    auto read = Aux(9, 15);
    replySize = 16;
    Submit(read);
    read = Aux();
    replySize = 0;
    Submit(read);
    Read();
    const auto requests = Named(L"AuxRequest");
    const auto replies = Named(L"AuxResponse");
    ASSERT_EQ(3U, requests.size());
    ASSERT_EQ(3U, replies.size());
    EXPECT_EQ(expectedWrite, requests[0].fields.at(L"Payload"));
    std::vector<BYTE> expectedRead{0x12, 0x34, 0x56};
    for (NvU32 i = 3; i < 16; ++i)
        expectedRead.push_back(static_cast<NvU8>(0x80 + i));
    EXPECT_EQ(expectedRead, replies[1].fields.at(L"Payload"));
    EXPECT_TRUE(replies[2].fields.at(L"Payload").empty());
    EXPECT_EQ(0U, replies[2].Number(L"ReportedLength"));
    EXPECT_EQ(1U, replies[2].Number(L"ValidLength"));
}

TEST_F(DpTrace, NackAndDeferReplyVariantsDoNotExposeReadPayloads)
{
    Enable(NV_DP_TRACE_AUX);
    const NvU32 types[] = {
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_NACK,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CNACK,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT,
    };
    for (NvU32 type : types)
    {
        replyType = type;
        auto data = Aux(5);
        Submit(data);
    }
    Read();
    const auto replies = Named(L"AuxResponse");
    ASSERT_EQ(5U, replies.size());
    for (unsigned i = 0; i < replies.size(); ++i)
    {
        EXPECT_EQ(types[i], replies[i].Number(L"ReplyType"));
        EXPECT_TRUE(replies[i].fields.at(L"Payload").empty());
    }
}

TEST_F(DpTrace, AuxFailuresAddressOnlyAndInvalidLengthsDoNotDumpStaleReadData)
{
    Enable(NV_DP_TRACE_AUX);
    operationStatus = NV_ERR_TIMEOUT;
    auto data = Aux();
    EXPECT_EQ(operationStatus, Submit(data));
    operationStatus = NV_OK;
    replyType = NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER;
    data = Aux();
    Submit(data);
    replyType = NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK;
    replySize = 0;
    data = Aux(4, 0);
    data.bAddrOnly = NV_TRUE;
    Submit(data);
    replySize = 17;
    data = Aux(8, 0xffffffff);
    Submit(data);
    Read();
    const auto requests = Named(L"AuxRequest");
    const auto replies = Named(L"AuxResponse");
    ASSERT_EQ(4U, requests.size());
    ASSERT_EQ(4U, replies.size());
    for (const auto& reply : replies)
        EXPECT_TRUE(reply.fields.at(L"Payload").empty());
    EXPECT_EQ(0U, requests[2].Number(L"RequestedLength"));
    EXPECT_EQ(0U, requests[3].Number(L"ValidLength"));
    EXPECT_EQ(0U, replies[3].Number(L"ValidLength"));
    EXPECT_EQ(17U, replies[0].Number(L"RetryTimeMs"));
}

TEST_F(DpTrace, FirmwareRecorderPreservesProvenanceRawTypesAndTimestampDomain)
{
    Enable(NV_DP_TRACE_RM_AUX);
    recorderCount = 1;
    auto data = Aux();
    Submit(data);
    EXPECT_EQ(1U, originalCalls);
    EXPECT_EQ(2U, recorderCalls);
    EXPECT_EQ(2U, lastSubdevice);
    Read();
    EXPECT_TRUE(Named(L"AuxRequest").empty());
    const auto records = Named(L"RmAuxRecord");
    ASSERT_EQ(2U, records.size());
    EXPECT_EQ("Firmware", records[0].Text(L"Origin"));
    EXPECT_EQ(NV_DP_AUXLOGGER_REQUEST_TYPE_MOTREQWSTAT, records[0].Number(L"RequestType"));
    EXPECT_EQ(500U, records[0].Number(L"RequestTimestamp"));
    EXPECT_EQ((std::vector<BYTE>{0xaa, 0xbb, 0xcc}), records[0].fields.at(L"Payload"));
}

TEST_F(DpTrace, RecorderFailuresAreExplicitAndNeverReplaceTheOperationStatus)
{
    Enable(NV_DP_TRACE_RM_AUX);
    recorderStatus = NV_ERR_NOT_SUPPORTED;
    recorderCount = 0;
    auto data = Aux();
    operationStatus = NV_ERR_TIMEOUT;
    EXPECT_EQ(operationStatus, Submit(data));
    Read();
    const auto polls = Named(L"RmAuxPoll");
    ASSERT_EQ(2U, polls.size());
    EXPECT_EQ(recorderStatus, polls[0].Number(L"Status"));
    EXPECT_EQ(0U, polls[0].Number(L"Valid"));
    EXPECT_TRUE(Named(L"RmAuxRecord").empty());
}

TEST_F(DpTrace, RecorderDrainIsBoundedAndMalformedResultsAreFlagged)
{
    Enable(NV_DP_TRACE_RM_AUX);
    recorderCount = MAX_LOGS_PER_POLL;
    recorderLength = 17;
    auto data = Aux();
    Submit(data);
    EXPECT_EQ(8U, recorderCalls);
    recorderCount = MAX_LOGS_PER_POLL + 1;
    data = Aux();
    Submit(data);
    EXPECT_EQ(10U, recorderCalls);
    Read();
    const auto polls = Named(L"RmAuxPoll");
    ASSERT_EQ(10U, polls.size());
    EXPECT_EQ(1U, polls[3].Number(L"DrainLimitReached"));
    EXPECT_EQ(0U, polls[8].Number(L"Valid"));
    const auto records = Named(L"RmAuxRecord");
    ASSERT_EQ(400U, records.size());
    EXPECT_EQ(0U, records[0].Number(L"ValidLength"));
    EXPECT_TRUE(records[0].fields.at(L"Payload").empty());
}
