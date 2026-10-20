#include <windows.h>
#include <winmeta.h>
#include <TraceLoggingProvider.h>

#include "nv-dp-tracelogging.h"
#include "dp_tracing.h"
#include "nvstatus.h"
#include <ctrl/ctrl0073/ctrl0073dp.h>

#include <atomic>
#include <cstring>

TRACELOGGING_DEFINE_PROVIDER(
    DpProvider,
    "NVIDIA.OpenGPU.DisplayPort",
    (0xf4b9d5d8, 0x9a13, 0x4b5f, 0xa2, 0x95, 0x5e, 0x61, 0x6c, 0x83, 0xc6, 0xc1));

namespace {

std::atomic<NvU64> transactionId{0};

UCHAR EventLevel(NvU32 priority)
{
    return priority == TRACE_DP_PRIORITY_ERROR ? WINEVENT_LEVEL_ERROR :
        priority == TRACE_DP_PRIORITY_WARNING ? WINEVENT_LEVEL_WARNING :
        WINEVENT_LEVEL_INFO;
}

NvU64 Qpc()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<NvU64>(value.QuadPart);
}

#define DP_WRITE(priority, name, ...) \
    do { \
        if ((priority) == TRACE_DP_PRIORITY_ERROR) \
            TraceLoggingWrite(DpProvider, name, TraceLoggingLevel(WINEVENT_LEVEL_ERROR), __VA_ARGS__); \
        else if ((priority) == TRACE_DP_PRIORITY_WARNING) \
            TraceLoggingWrite(DpProvider, name, TraceLoggingLevel(WINEVENT_LEVEL_WARNING), __VA_ARGS__); \
        else \
            TraceLoggingWrite(DpProvider, name, TraceLoggingLevel(WINEVENT_LEVEL_INFO), __VA_ARGS__); \
    } while (0)

void InvalidEvent(NvU32 event, NvU32 priority, NvU32 count)
{
    TraceLoggingWrite(DpProvider, "InvalidDpEvent",
        TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
        TraceLoggingKeyword(NV_DP_TRACE_EVENTS),
        TraceLoggingString("Driver", "Origin"),
        TraceLoggingUInt32(event, "EventId"),
        TraceLoggingUInt32(priority, "Priority"),
        TraceLoggingUInt32(count, "ArgumentCount"));
}

void RecorderPoll(
    NvU32 client, NvU32 object, NvU32 subdevice, NV_DP_TRACE_RM_CONTROL control)
{
    // Activity-driven, bounded reads: tracing must not spin on an active producer.
    constexpr NvU32 MaxPolls = 4;
    for (NvU32 poll = 0; poll < MaxPolls; ++poll)
    {
        if (!TraceLoggingProviderEnabled(DpProvider, WINEVENT_LEVEL_VERBOSE, NV_DP_TRACE_RM_AUX))
            return;

        NV0073_CTRL_CMD_DP_GET_AUXLOGGER_BUFFER_DATA_PARAMS data = {};
        data.subDeviceInstance = subdevice;
        data.dpAuxBufferReadSize = MAX_LOGS_PER_POLL;
        const NvU32 status = control(client, object,
            NV0073_CTRL_CMD_DP_GET_AUXLOGGER_BUFFER_DATA, &data, sizeof(data));
        const bool valid = status == NV_OK && data.dpNumMessagesRead <= MAX_LOGS_PER_POLL;
        TraceLoggingWrite(DpProvider, "RmAuxPoll",
            TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
            TraceLoggingKeyword(NV_DP_TRACE_RM_AUX),
            TraceLoggingString("Driver", "Origin"),
            TraceLoggingUInt32(client, "Client"),
            TraceLoggingUInt32(object, "Object"),
            TraceLoggingUInt32(subdevice, "Subdevice"),
            TraceLoggingHexUInt32(status, "Status"),
            TraceLoggingBool(valid, "Valid"),
            TraceLoggingUInt32(data.dpNumMessagesRead, "RecordCount"),
            TraceLoggingBool(valid && data.dpNumMessagesRead == MAX_LOGS_PER_POLL &&
                poll + 1 == MaxPolls, "DrainLimitReached"));
        if (!valid)
            return;

        for (NvU32 index = 0; index < data.dpNumMessagesRead; ++index)
        {
            const DPAUXPACKET& packet = data.dpAuxBuffer[index];
            const bool validLength = packet.auxMessageReplySize <= DP_MAX_MSG_SIZE;
            // Preserve the recorder's payload semantics; it has only one byte array.
            TraceLoggingWrite(DpProvider, "RmAuxRecord",
                TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                TraceLoggingKeyword(NV_DP_TRACE_RM_AUX),
                TraceLoggingString("Firmware", "Origin"),
                TraceLoggingString("PhysicalRmAuxRecorder", "CapturePoint"),
                TraceLoggingUInt32(client, "Client"),
                TraceLoggingUInt32(object, "Object"),
                TraceLoggingUInt32(subdevice, "Subdevice"),
                TraceLoggingUInt32(packet.auxCount, "Sequence"),
                TraceLoggingHexUInt32(packet.auxEvents, "Events"),
                TraceLoggingUInt32((packet.auxEvents >> 8) & 3U, "EventType"),
                TraceLoggingUInt32(packet.auxEvents & 15U, "RequestType"),
                TraceLoggingUInt32((packet.auxEvents >> 4) & 15U, "ReplyType"),
                TraceLoggingUInt32(packet.auxOutPort, "Port"),
                TraceLoggingHexUInt32(packet.auxPortAddress, "Address"),
                TraceLoggingUInt32(packet.auxRequestTimeStamp, "RequestTimestamp"),
                TraceLoggingUInt32(packet.auxReplyTimeStamp, "ReplyTimestamp"),
                TraceLoggingUInt32(packet.auxMessageReqSize, "RequestedLength"),
                TraceLoggingUInt32(packet.auxMessageReplySize, "ReportedLength"),
                TraceLoggingBool(validLength, "ValidLength"),
                TraceLoggingBinary(packet.auxPacket,
                    validLength ? packet.auxMessageReplySize : 0U, "Payload"));
        }
        if (data.dpNumMessagesRead < MAX_LOGS_PER_POLL)
            return;
    }
}

} // namespace

extern "C" NvU32 NvDpTraceInitialize(void)
{
    return TraceLoggingRegister(DpProvider);
}

extern "C" void NvDpTraceShutdown(void)
{
    TraceLoggingUnregister(DpProvider);
}

extern "C" void NvDpTraceText(NvU32 level, const char* gpuPrefix, const char* message)
{
    const NvU32 priority = level == 2 ? TRACE_DP_PRIORITY_ERROR :
        level == 1 ? TRACE_DP_PRIORITY_WARNING : TRACE_DP_PRIORITY_INFO;
    if (!TraceLoggingProviderEnabled(DpProvider, EventLevel(priority), NV_DP_TRACE_TEXT))
        return;

    if (message == nullptr)
        message = "";
    if (gpuPrefix == nullptr)
        gpuPrefix = "";
    const NvU64 id = ++transactionId;
    NvU32 chunkIndex = 0;
    do
    {
        char chunk[4096];
        size_t size = 0;
        while (size < sizeof(chunk) - 1 && message[size] != '\0')
            ++size;
        std::memcpy(chunk, message, size);
        chunk[size] = '\0';
        message += size;
        DP_WRITE(priority, "DpText",
            TraceLoggingKeyword(NV_DP_TRACE_TEXT),
            TraceLoggingString("Driver", "Origin"),
            TraceLoggingString("NVKMS", "Component"),
            TraceLoggingUInt32(level, "NvKmsLevel"),
            TraceLoggingString(gpuPrefix, "GpuPrefix"),
            TraceLoggingUInt64(id, "MessageId"),
            TraceLoggingUInt32(chunkIndex++, "ChunkIndex"),
            TraceLoggingBool(*message == '\0', "LastChunk"),
            TraceLoggingString(chunk, "Message"));
    } while (*message != '\0');
}

extern "C" NvBool NvDpTraceEventEnabled(NvU32 priority)
{
    return TraceLoggingProviderEnabled(DpProvider, EventLevel(priority), NV_DP_TRACE_EVENTS);
}

extern "C" void NvDpTraceEvent(
    NvU32 event, NvU32 priority, NvU32 count, const NvU64* args)
{
    if (!NvDpTraceEventEnabled(priority))
        return;

    static constexpr NvU32 Counts[] = {3, 8, 5, 8, 5, 2, 7, 3, 7, 1, 0, 0, 0, 6};
    if (event >= sizeof(Counts) / sizeof(Counts[0]) || count != Counts[event] ||
        priority > TRACE_DP_PRIORITY_INFO || (count != 0 && args == nullptr))
    {
        InvalidEvent(event, priority, count);
        return;
    }

#define DP_EVENT(name, ...) \
    DP_WRITE(priority, name, TraceLoggingKeyword(NV_DP_TRACE_EVENTS), \
        TraceLoggingString("Driver", "Origin"), TraceLoggingUInt32(event, "EventId"), \
        TraceLoggingUInt32(priority, "Priority"), ##__VA_ARGS__)
#define DP_ADDRESS(offset) \
    TraceLoggingUInt64(args[offset], "AddressLength"), \
    TraceLoggingUInt64(args[offset + 1], "Address0"), \
    TraceLoggingUInt64(args[offset + 2], "Address1"), \
    TraceLoggingUInt64(args[offset + 3], "Address2"), \
    TraceLoggingUInt64(args[offset + 4], "Address3")

    switch (event)
    {
    case TRACE_DP_ID_HOTPLUG:
        DP_EVENT("Hotplug", TraceLoggingBool(args[0] != 0, "Connected"),
            TraceLoggingBool(args[1] != 0, "ConnectorActive"),
            TraceLoggingBool(args[2] != 0, "PreviouslyPlugged"));
        break;
    case TRACE_DP_ID_NEW_SINK_DETECTED:
        DP_EVENT("NewSinkDetected", DP_ADDRESS(0),
            TraceLoggingBool(args[5] != 0, "Multistream"),
            TraceLoggingUInt64(args[6], "ManufacturerId"),
            TraceLoggingUInt64(args[7], "ProductId"));
        break;
    case TRACE_DP_ID_NEW_SINK_REPORTED:
        DP_EVENT("NewSinkReported", DP_ADDRESS(0));
        break;
    case TRACE_DP_ID_NEW_MST_DEVICE:
        DP_EVENT("NewMstDevice", DP_ADDRESS(0),
            TraceLoggingBool(args[5] != 0, "Branch"),
            TraceLoggingBool(args[6] != 0, "Legacy"),
            TraceLoggingUInt64(args[7], "PeerDevice"));
        break;
    case TRACE_DP_ID_LOST_DEVICE:
        DP_EVENT("LostDevice", DP_ADDRESS(0));
        break;
    case TRACE_DP_ID_LINK_ASSESSMENT:
        DP_EVENT("LinkAssessment", TraceLoggingUInt64(args[0], "PeakRate"),
            TraceLoggingUInt64(args[1], "Lanes"));
        break;
    case TRACE_DP_ID_LINK_TRAINING_START:
        DP_EVENT("LinkTrainingStart", TraceLoggingBool(args[0] != 0, "Multistream"),
            TraceLoggingUInt64(args[1], "PeakRate"),
            TraceLoggingUInt64(args[2], "Lanes"),
            TraceLoggingUInt64(args[3], "PhyRepeaterCount"),
            TraceLoggingUInt64(args[4], "RmPhyRepeaterCount"),
            TraceLoggingBool(args[5] != 0, "TrainPhyRepeater"),
            TraceLoggingUInt64(args[6], "Target"));
        break;
    case TRACE_DP_ID_LINK_TRAINING_DONE:
        DP_EVENT("LinkTrainingDone", TraceLoggingUInt64(args[0], "Status"),
            TraceLoggingUInt64(args[1], "PeakRate"),
            TraceLoggingUInt64(args[2], "Lanes"));
        break;
    case TRACE_DP_ID_NOTIFY_ATTACH_BEGIN:
        DP_EVENT("AttachBegin", TraceLoggingUInt64(args[0], "Head"),
            TraceLoggingUInt64(args[1], "PixelClockHz"),
            TraceLoggingUInt64(args[2], "RasterWidth"),
            TraceLoggingUInt64(args[3], "RasterHeight"),
            TraceLoggingUInt64(args[4], "Depth"),
            TraceLoggingBool(args[5] != 0, "Dsc"),
            TraceLoggingBool(args[6] != 0, "Fec"));
        break;
    case TRACE_DP_ID_NOTIFY_ATTACH_BEGIN_STATUS:
        DP_EVENT("AttachStatus", TraceLoggingBool(args[0] != 0, "LinkTrainingSucceeded"));
        break;
    case TRACE_DP_ID_NOTIFY_ATTACH_END:
        DP_EVENT("AttachEnd");
        break;
    case TRACE_DP_ID_NOTIFY_DETACH_BEGIN:
        DP_EVENT("DetachBegin");
        break;
    case TRACE_DP_ID_NOTIFY_DETACH_END:
        DP_EVENT("DetachEnd");
        break;
    case TRACE_DP_ID_MESSAGE_EXPIRED:
        DP_EVENT("MessageExpired", TraceLoggingUInt64(args[0], "RequestId"), DP_ADDRESS(1));
        break;
    }
#undef DP_ADDRESS
#undef DP_EVENT
}

extern "C" NvU32 NvDpTraceControl(
    NvU32 client, NvU32 object, NvU32 command, void* params, NvU32 paramsSize,
    NV_DP_TRACE_RM_CONTROL control)
{
    const bool aux = command == NV0073_CTRL_CMD_DP_AUXCH_CTRL && params != nullptr &&
        paramsSize == sizeof(NV0073_CTRL_DP_AUXCH_CTRL_PARAMS);
    const bool lt = command == NV0073_CTRL_CMD_DP_CTRL && params != nullptr &&
        paramsSize == sizeof(NV0073_CTRL_DP_CTRL_PARAMS);
    const bool lt2 = command == NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL && params != nullptr &&
        paramsSize == sizeof(NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL_PARAMS);
    const bool captureAux = aux &&
        TraceLoggingProviderEnabled(DpProvider, WINEVENT_LEVEL_VERBOSE, NV_DP_TRACE_AUX);
    const bool captureLt = (lt || lt2) &&
        TraceLoggingProviderEnabled(DpProvider, WINEVENT_LEVEL_INFO, NV_DP_TRACE_EVENTS);
    const bool recorder = (aux || lt || lt2) &&
        TraceLoggingProviderEnabled(DpProvider, WINEVENT_LEVEL_VERBOSE, NV_DP_TRACE_RM_AUX);
    if (!captureAux && !captureLt && !recorder)
        return control(client, object, command, params, paramsSize);

    NvU32 subdevice;
    NvU32 display;
    if (aux)
    {
        const auto* data = static_cast<NV0073_CTRL_DP_AUXCH_CTRL_PARAMS*>(params);
        subdevice = data->subDeviceInstance;
        display = data->displayId;
    }
    else if (lt)
    {
        const auto* data = static_cast<NV0073_CTRL_DP_CTRL_PARAMS*>(params);
        subdevice = data->subDeviceInstance;
        display = data->displayId;
    }
    else
    {
        const auto* data = static_cast<NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL_PARAMS*>(params);
        subdevice = data->subDeviceInstance;
        display = data->displayId;
    }
    if (recorder)
        RecorderPoll(client, object, subdevice, control);

    const NvU64 id = ++transactionId;
    const NvU64 begin = Qpc();
    NV0073_CTRL_DP_AUXCH_CTRL_PARAMS request = {};
    if (captureAux)
    {
        request = *static_cast<NV0073_CTRL_DP_AUXCH_CTRL_PARAMS*>(params);
        const bool validLength = request.bAddrOnly ? request.size == 0 :
            request.size < NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE;
        const NvU32 requestType = request.cmd & 3U;
        const bool write = requestType == NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE;
        const NvU32 length = !request.bAddrOnly && validLength &&
            requestType != NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE_STATUS ? request.size + 1 : 0;
        TraceLoggingWrite(DpProvider, "AuxRequest",
            TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
            TraceLoggingKeyword(NV_DP_TRACE_AUX),
            TraceLoggingString("Driver", "Origin"),
            TraceLoggingString("RmControlBoundary", "CapturePoint"),
            TraceLoggingUInt64(id, "TransactionId"),
            TraceLoggingUInt64(begin, "Qpc"),
            TraceLoggingUInt32(client, "Client"),
            TraceLoggingUInt32(object, "Object"),
            TraceLoggingUInt32(subdevice, "Subdevice"),
            TraceLoggingHexUInt32(display, "DisplayId"),
            TraceLoggingHexUInt32(request.cmd, "Command"),
            TraceLoggingUInt32(requestType, "RequestType"),
            TraceLoggingBool((request.cmd & 8U) != 0, "NativeAux"),
            TraceLoggingBool((request.cmd & 8U) == 0 && (request.cmd & 4U) != 0,
                "MiddleOfTransaction"),
            TraceLoggingHexUInt32(request.addr, "Address"),
            TraceLoggingBool(request.bAddrOnly != 0, "AddressOnly"),
            TraceLoggingUInt32(request.size, "RawSize"),
            TraceLoggingUInt32(length, "RequestedLength"),
            TraceLoggingBool(validLength, "ValidLength"),
            TraceLoggingBinary(request.data, write ? length : 0U, "Payload"));
    }
    if (captureLt)
    {
        const NvU32 cmd = lt ? static_cast<NV0073_CTRL_DP_CTRL_PARAMS*>(params)->cmd :
            static_cast<NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL_PARAMS*>(params)->cmd;
        const NvU32 data = lt ? static_cast<NV0073_CTRL_DP_CTRL_PARAMS*>(params)->data :
            static_cast<NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL_PARAMS*>(params)->data;
        TraceLoggingWrite(DpProvider, "LinkControlRequest",
            TraceLoggingLevel(WINEVENT_LEVEL_INFO),
            TraceLoggingKeyword(NV_DP_TRACE_EVENTS),
            TraceLoggingString("Driver", "Origin"),
            TraceLoggingUInt64(id, "TransactionId"),
            TraceLoggingUInt64(begin, "Qpc"),
            TraceLoggingUInt32(client, "Client"),
            TraceLoggingUInt32(object, "Object"),
            TraceLoggingUInt32(subdevice, "Subdevice"),
            TraceLoggingHexUInt32(display, "DisplayId"),
            TraceLoggingHexUInt32(command, "Control"),
            TraceLoggingHexUInt32(cmd, "Command"),
            TraceLoggingHexUInt32(data, "Data"));
    }
    const NvU32 status = control(client, object, command, params, paramsSize);
    const NvU64 end = Qpc();
    if (captureAux)
    {
        const auto& reply = *static_cast<NV0073_CTRL_DP_AUXCH_CTRL_PARAMS*>(params);
        const bool read = (request.cmd & 3U) == NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_READ;
        const bool validLength = reply.size <= NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE &&
            (!read || (request.size < NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE &&
                       reply.size <= request.size + 1));
        const NvU32 length = status == NV_OK &&
            reply.replyType == NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK &&
            read && !request.bAddrOnly && validLength ? reply.size : 0;
        TraceLoggingWrite(DpProvider, "AuxResponse",
            TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
            TraceLoggingKeyword(NV_DP_TRACE_AUX),
            TraceLoggingString("Driver", "Origin"),
            TraceLoggingUInt64(id, "TransactionId"),
            TraceLoggingUInt64(end, "Qpc"),
            TraceLoggingUInt64(end - begin, "DurationQpc"),
            TraceLoggingHexUInt32(status, "Status"),
            TraceLoggingBool(status == NV_OK, "ReplyValid"),
            TraceLoggingUInt32(reply.replyType, "ReplyType"),
            TraceLoggingUInt32(reply.size, "ReportedLength"),
            TraceLoggingUInt32(reply.retryTimeMs, "RetryTimeMs"),
            TraceLoggingBool(validLength, "ValidLength"),
            TraceLoggingBinary(reply.data, length, "Payload"));
    }
    if (captureLt)
    {
        const NvU32 error = lt ? static_cast<NV0073_CTRL_DP_CTRL_PARAMS*>(params)->err :
            static_cast<NV0073_CTRL_CMD_DP2X_LINK_TRAINING_CTRL_PARAMS*>(params)->err;
        TraceLoggingWrite(DpProvider, "LinkControlResponse",
            TraceLoggingLevel(WINEVENT_LEVEL_INFO),
            TraceLoggingKeyword(NV_DP_TRACE_EVENTS),
            TraceLoggingString("Driver", "Origin"),
            TraceLoggingUInt64(id, "TransactionId"),
            TraceLoggingUInt64(end, "Qpc"),
            TraceLoggingUInt64(end - begin, "DurationQpc"),
            TraceLoggingHexUInt32(status, "Status"),
            TraceLoggingHexUInt32(error, "Error"));
    }
    if (recorder)
        RecorderPoll(client, object, subdevice, control);
    return status;
}
