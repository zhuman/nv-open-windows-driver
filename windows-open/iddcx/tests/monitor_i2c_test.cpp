#include <gtest/gtest.h>

#include "../../modeset/monitor_i2c.hpp"
#include "../../modeset/monitor_i2c_aux.hpp"
#include "../../modeset/monitor_i2c_native.hpp"
#include <vector>

using namespace nv::monitor_i2c;

struct Operation
{
    char type;
    NvU32 size;
    bool more;
};

struct Bus
{
    std::vector<Operation> operations;
    std::vector<NvU8> reply;
    size_t readOffset = 0;
    unsigned failOperation = 0;
    NV_STATUS finishStatus = NV_OK;
    NV_STATUS Begin() noexcept { return Record('B', 0, false); }
    NV_STATUS Write(NvU32 address, const NvU8*, NvU32 size, bool more) noexcept
    {
        EXPECT_TRUE(address == DdcCiAddress);
        return Record('W', size, more);
    }
    NV_STATUS Read(NvU32 address, NvU8* data, NvU32 size, bool more) noexcept
    {
        EXPECT_TRUE(address == DdcCiAddress);
        const auto status = Record('R', size, more);
        if (status == NV_OK)
        {
            if (readOffset > reply.size() || size > reply.size() - readOffset)
            {
                ADD_FAILURE() << "Fake bus read exceeds its reply";
                return NV_ERR_I2C_ERROR;
            }
            std::memcpy(data, reply.data() + readOffset, size);
            readOffset += size;
        }
        return status;
    }
    NV_STATUS Finish() noexcept
    {
        Record('F', 0, false);
        return finishStatus;
    }
    NV_STATUS Record(char type, NvU32 size, bool more) noexcept
    {
        operations.push_back({type, size, more});
        return operations.size() == failOperation ? NV_ERR_I2C_ERROR : NV_OK;
    }
};

struct AuxReply
{
    NV_STATUS status = NV_OK;
    NvU32 reply = NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK;
    NvU32 size = ~0U;
    NvU32 delay = 0;
};

struct AuxBackend
{
    std::vector<NV0073_CTRL_DP_AUXCH_CTRL_PARAMS> requests;
    std::vector<AuxReply> replies;
    NvU32 limit = 16;
    bool expired = false;
    bool allowDelay = true;
    unsigned waits = 0;
    NV_STATUS SubmitAux(NV0073_CTRL_DP_AUXCH_CTRL_PARAMS* parameters) noexcept
    {
        requests.push_back(*parameters);
        EXPECT_TRUE(parameters->displayId == 0x200 && parameters->addr == DdcCiAddress);
        EXPECT_TRUE(parameters->size < 16);
        const size_t index = requests.size() - 1;
        const AuxReply reply = index < replies.size() ? replies[index] : AuxReply{};
        parameters->replyType = reply.reply;
        parameters->retryTimeMs = reply.delay;
        const NvU32 count = parameters->bAddrOnly ? 0 : parameters->size + 1;
        parameters->size = reply.size != ~0U ? reply.size : count < limit ? count : limit;
        std::memset(parameters->data, 0xa5, sizeof(parameters->data));
        return reply.status;
    }
    bool AuxExpired() const noexcept { return expired; }
    bool WaitAux(NvU32 milliseconds) noexcept
    {
        ++waits;
        return allowDelay && !expired && milliseconds <= 20;
    }
};

class MonitorAux : public ::testing::Test
{
protected:
    NvU8 data[MaxReceive] = {};
};

TEST_F(MonitorAux, ChunksReadsAndWritesAcrossAllSizesAndPartialAckLimits)
{
    // AUX encodes length minus one; MOT (bit 2) keeps the I2C transaction open
    // across chunk boundaries, including partial acknowledgements.
    for (NvU32 size = 1; size <= MaxReceive; ++size)
    {
        for (NvU32 limit = 1; limit <= 16; ++limit)
        {
            for (bool read : {false, true})
            {
                AuxBackend backend;
                backend.limit = limit;
                AuxBus bus(backend, 0x200);
                EXPECT_TRUE((read ? bus.Read(DdcCiAddress, data, size, true) :
                    bus.Write(DdcCiAddress, data, size, true)) == NV_OK);
                NvU32 completed = 0;
                for (const auto& request : backend.requests)
                {
                    EXPECT_TRUE(!request.bAddrOnly && (request.cmd & 4U) != 0);
                    EXPECT_TRUE((request.cmd & 3U) == (read ? 1U : 0U));
                    EXPECT_TRUE(request.size + 1 <= size - completed);
                    completed += request.size + 1 < limit ? request.size + 1 : limit;
                }
                EXPECT_TRUE(completed == size);
                EXPECT_TRUE(bus.Finish() == NV_OK);
                ASSERT_FALSE(backend.requests.empty());
                const auto& stop = backend.requests.back();
                EXPECT_TRUE(stop.bAddrOnly && stop.size == 0 && stop.cmd == 0);
                const size_t before = backend.requests.size();
                EXPECT_TRUE(bus.Finish() == NV_OK && backend.requests.size() == before);
            }
        }
    }
}

TEST_F(MonitorAux, FinalReadChunkSendsStopExactlyOnce)
{
    for (NvU32 size = 1; size <= MaxReceive; ++size)
    {
        AuxBackend backend;
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, size) == NV_OK);
        ASSERT_FALSE(backend.requests.empty());
        EXPECT_TRUE(backend.requests.back().cmd == 1);
        for (size_t index = 0; index + 1 < backend.requests.size(); ++index)
        {
            EXPECT_TRUE(backend.requests[index].cmd == 5);
        }
        const size_t count = backend.requests.size();
        EXPECT_TRUE(bus.Finish() == NV_OK && backend.requests.size() == count);
    }
}

TEST_F(MonitorAux, CombinedWriteReadPreservesTransaction)
{
    {
        AuxBackend backend;
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Write(DdcCiAddress, data, 5, true) == NV_OK);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 11) == NV_OK);
        EXPECT_TRUE(backend.requests.size() == 2 && backend.requests[0].cmd == 4 &&
            backend.requests[1].cmd == 1);
        EXPECT_TRUE(bus.Finish() == NV_OK && backend.requests.size() == 2);
    }
}

TEST_F(MonitorAux, RejectsPartialFinalRead)
{
    // The final read already sent STOP, so a short ACK cannot be resumed as
    // though it were another chunk of the same transaction.
    {
        AuxBackend backend;
        backend.limit = 2;
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 5) == NV_ERR_I2C_ERROR);
        EXPECT_TRUE(bus.Finish() == NV_OK && backend.requests.size() == 1);
    }
}

TEST_F(MonitorAux, RetriesDeferAndI2cWriteStatus)
{
    // I2CDEFER retries WRITE_STATUS_UPDATE, not the original payload, to avoid
    // duplicating a write whose completion is still pending.
    for (const NvU32 reply : {NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER})
    {
        AuxBackend backend;
        backend.replies = {{NV_OK, reply}, {NV_OK, reply}};
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Write(DdcCiAddress, data, 5) == NV_OK && backend.waits == 2);
        ASSERT_GE(backend.requests.size(), 3U);
        EXPECT_TRUE(backend.requests[0].cmd == 0);
        EXPECT_TRUE(backend.requests[1].cmd ==
            (reply == NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER ? 2U : 0U));
        EXPECT_TRUE(backend.requests[2].cmd == backend.requests[1].cmd);
        EXPECT_TRUE(bus.Finish() == NV_OK);
    }
}

TEST_F(MonitorAux, PropagatesNackAndTimeoutAndFinishesTransaction)
{
    for (const NvU32 reply : {NV0073_CTRL_DP_AUXCH_REPLYTYPE_NACK,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CNACK,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT})
    {
        AuxBackend backend;
        backend.replies = {{NV_OK, reply}};
        AuxBus bus(backend, 0x200);
        const NV_STATUS expected = reply == NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT ?
            NV_ERR_TIMEOUT : NV_ERR_OBJECT_NOT_FOUND;
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 10) == expected);
        EXPECT_TRUE(bus.Finish() == NV_OK && backend.requests.back().bAddrOnly);
    }
}

TEST_F(MonitorAux, RejectsMalformedAckCounts)
{
    for (NvU32 count : {0U, 17U})
    {
        AuxBackend backend;
        backend.replies = {{NV_OK, NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK, count}};
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 16) == NV_ERR_I2C_ERROR);
        EXPECT_TRUE(bus.Finish() == NV_OK);
    }
}

TEST_F(MonitorAux, BoundsDeferRetries)
{
    {
        AuxBackend backend;
        backend.replies.resize(7, {NV_OK, NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER});
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 10) == NV_ERR_TIMEOUT);
        EXPECT_TRUE(backend.requests.size() == 7 && backend.waits == 6);
        EXPECT_TRUE(bus.Finish() == NV_OK);
    }
}

TEST_F(MonitorAux, RejectsExcessiveBusyRetryDelay)
{
    {
        AuxBackend backend;
        backend.replies = {{NV_ERR_BUSY_RETRY, 0, ~0U, 21}};
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 10) == NV_ERR_TIMEOUT);
        EXPECT_TRUE(backend.requests.size() == 1 && bus.Finish() == NV_OK);
    }
}

TEST_F(MonitorAux, PreservesBackendError)
{
    {
        AuxBackend backend;
        backend.replies = {{NV_ERR_INVALID_DATA}};
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 10) == NV_ERR_INVALID_DATA);
        EXPECT_TRUE(bus.Finish() == NV_OK);
    }
}

TEST_F(MonitorAux, ExpiredDeadlineSkipsBackend)
{
    {
        AuxBackend backend;
        backend.expired = true;
        AuxBus bus(backend, 0x200);
        EXPECT_TRUE(bus.Read(DdcCiAddress, data, 10) == NV_ERR_TIMEOUT);
        EXPECT_TRUE(backend.requests.empty() && bus.Finish() == NV_OK);
    }
}

struct NativeBackend
{
    unsigned calls = 0;
    NV_STATUS status = NV_OK;
    NV402C_CTRL_I2C_INDEXED_PARAMS recorded = {};
    NV_STATUS SubmitNative(NV402C_CTRL_I2C_INDEXED_PARAMS* parameters) noexcept
    {
        ++calls;
        recorded = *parameters;
        EXPECT_TRUE(parameters->address == 0x6e && parameters->flags == 0);
        EXPECT_TRUE(parameters->indexLength <= 4);
        if (!parameters->bIsWrite)
        {
            std::memset(parameters->pMessage, 0xa5, parameters->messageLength);
        }
        return status;
    }
};

TEST(MonitorNative, IndexedTransfersAcrossAllPortsSizesAndPrefixes)
{
    // The control ABI uses zero-based ports and at most four index bytes;
    // an unsupported prefix must be rejected before any backend traffic.
    NvU8 transmit[MaxTransmit] = {0x51, 0x82, 0x01, 0x10};
    NvU8 receive[MaxReceive] = {};
    Request request = {DdcCiAddress, 0, transmit, DdcCiAddress, 0, receive, 0};
    for (NvU32 port = 1; port <= 16; ++port)
    {
        for (NvU32 size = 1; size <= MaxTransmit; ++size)
        {
            NativeBackend backend;
            request.transmitSize = size;
            request.receiveSize = 0;
            EXPECT_TRUE(IndexedTransfer(backend, port, request) == NV_OK && backend.calls == 1);
            EXPECT_TRUE(backend.recorded.bIsWrite && backend.recorded.portId == port - 1 &&
                backend.recorded.messageLength == size && backend.recorded.indexLength == 0);
        }
        for (NvU32 size = 1; size <= MaxReceive; ++size)
        {
            for (NvU32 prefix = 0; prefix <= 5; ++prefix)
            {
                NativeBackend backend;
                request.transmitSize = prefix;
                request.receiveSize = size;
                std::memset(receive, 0xcc, sizeof(receive));
                const NV_STATUS expected = prefix <= 4 ? NV_OK : NV_ERR_NOT_SUPPORTED;
                EXPECT_TRUE(IndexedTransfer(backend, port, request) == expected);
                if (prefix <= 4)
                {
                    EXPECT_TRUE(backend.calls == 1 && !backend.recorded.bIsWrite &&
                        backend.recorded.portId == port - 1 &&
                        backend.recorded.messageLength == size &&
                        backend.recorded.indexLength == prefix);
                    EXPECT_TRUE(std::memcmp(backend.recorded.index, transmit, prefix) == 0);
                    for (NvU32 index = 0; index < size; ++index) { EXPECT_TRUE(receive[index] == 0xa5); }
                    for (NvU32 index = size; index < MaxReceive; ++index) { EXPECT_TRUE(receive[index] == 0xcc); }
                }
                else { EXPECT_TRUE(backend.calls == 0 && receive[0] == 0xcc); }
            }
        }
    }
}

TEST(MonitorNative, RejectsInvalidFlagsPortsAndPreservesReplyOnError)
{
    // The fake writes before returning an error; the caller must never see
    // that partial result as a successfully received reply.
    NvU8 transmit[MaxTransmit] = {0x51, 0x82, 0x01, 0x10};
    NvU8 receive[MaxReceive] = {};
    Request request = {DdcCiAddress, 0, transmit, DdcCiAddress, 0, receive, 0};
    request.transmitSize = 0;
    request.receiveSize = 16;
    request.receiveFlags = 1;
    NativeBackend backend;
    EXPECT_TRUE(IndexedTransfer(backend, 1, request) == NV_ERR_NOT_SUPPORTED && backend.calls == 0);
    request.receiveFlags = 0;
    EXPECT_TRUE(IndexedTransfer(backend, 0, request) == NV_ERR_INVALID_ARGUMENT && backend.calls == 0);
    EXPECT_TRUE(IndexedTransfer(backend, 17, request) == NV_ERR_INVALID_ARGUMENT && backend.calls == 0);
    backend.status = NV_ERR_I2C_ERROR;
    std::memset(receive, 0xcc, sizeof(receive));
    EXPECT_TRUE(IndexedTransfer(backend, 1, request) == NV_ERR_I2C_ERROR && receive[0] == 0xcc);
}

class MonitorI2c : public ::testing::Test
{
protected:
    NvU8 transmit[MaxTransmit] = {};
    NvU8 receive[MaxReceive] = {};
    Request request = { DdcCiAddress, 3, transmit, DdcCiAddress, MaxReceive, receive, 1 };
};

TEST(MonitorI2cAddress, NormalizesOnlyWindowsDdcCiAddresses)
{
    // Windows exposes 8-bit write/read forms; the transport uses a 7-bit address.
    for (NvU32 address = 0; address <= 255; ++address)
    {
        EXPECT_TRUE(WindowsAddress(address, false) == (address == 0x6eU ? DdcCiAddress : address));
        EXPECT_TRUE(WindowsAddress(address, true) == (address == 0x6fU ? DdcCiAddress : address));
    }
}

TEST_F(MonitorI2c, FramesAllPayloadLengthsAndReceiveCapacities)
{
    // Probe the two-byte DDC/CI header before sizing the body. Insufficient
    // capacity must leave the caller's buffer intact, not publish the header.
    for (NvU32 payload = 0; payload <= 127; ++payload)
    {
        for (NvU32 capacity = 3; capacity <= MaxReceive; ++capacity)
        {
            Bus bus;
            bus.reply.resize(payload + 3, 0xa5);
            bus.reply[0] = 0x6e;
            bus.reply[1] = static_cast<NvU8>(payload | 0x80);
            std::memset(receive, 0xcc, sizeof(receive));
            request.receiveSize = capacity;
            const NV_STATUS status = Transfer(bus, request);
            ASSERT_GE(bus.operations.size(), 4U);
            EXPECT_TRUE(bus.operations.front().type == 'B');
            EXPECT_TRUE(bus.operations[1].type == 'W' && bus.operations[1].more);
            EXPECT_TRUE(bus.operations[2].type == 'R' && bus.operations[2].size == 2 &&
                bus.operations[2].more);
            EXPECT_TRUE(bus.operations.back().type == 'F');
            if (capacity < payload + 3)
            {
                EXPECT_TRUE(status == NV_ERR_BUFFER_TOO_SMALL && bus.readOffset == 2);
                for (auto byte : receive) { EXPECT_TRUE(byte == 0xcc); }
            }
            else
            {
                EXPECT_TRUE(status == NV_OK && bus.readOffset == payload + 3);
                ASSERT_GE(bus.operations.size(), 5U);
                EXPECT_TRUE(!bus.operations[3].more && bus.operations[3].size == payload + 1);
                EXPECT_TRUE(std::memcmp(receive, bus.reply.data(), payload + 3) == 0);
                for (NvU32 index = payload + 3; index < capacity; ++index)
                {
                    EXPECT_TRUE(receive[index] == 0);
                }
                for (NvU32 index = capacity; index < MaxReceive; ++index)
                {
                    EXPECT_TRUE(receive[index] == 0xcc);
                }
            }
        }
    }
}

TEST_F(MonitorI2c, PreservesReceiveBufferAndFinishesOnTransferErrors)
{
    request.receiveSize = 16;
    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        Bus bus;
        bus.reply = { 0x6e, 0x80, 0xbe };
        bus.failOperation = failure;
        std::memset(receive, 0xcc, sizeof(receive));
        EXPECT_TRUE(Transfer(bus, request) == NV_ERR_I2C_ERROR);
        EXPECT_TRUE(failure == 1 || bus.operations.back().type == 'F');
        for (auto byte : receive) { EXPECT_TRUE(byte == 0xcc); }
    }
    {
        Bus bus;
        bus.reply = { 0x6e, 0x80, 0xbe };
        bus.finishStatus = NV_ERR_TIMEOUT;
        EXPECT_TRUE(Transfer(bus, request) == NV_ERR_TIMEOUT);
    }
}

TEST_F(MonitorI2c, RejectsUnsupportedAddresses)
{
    request.receiveSize = 16;
    for (NvU32 address = 0; address <= 255; ++address)
    {
        const NV_STATUS expected = address == DdcCiAddress ? NV_OK : NV_ERR_NOT_SUPPORTED;
        request.transmitAddress = address;
        EXPECT_TRUE(Validate(request) == expected);
        request.transmitAddress = DdcCiAddress;
        request.receiveAddress = address;
        EXPECT_TRUE(Validate(request) == expected);
        request.receiveAddress = DdcCiAddress;
    }
}

TEST_F(MonitorI2c, RejectsInvalidReceiveFlagsWithoutBusTraffic)
{
    request.receiveSize = 16;
    for (NvU32 flags = 2; flags < 256; ++flags)
    {
        request.receiveFlags = flags;
        Bus bus;
        EXPECT_TRUE(Transfer(bus, request) == NV_ERR_INVALID_ARGUMENT && bus.operations.empty());
    }
}

TEST_F(MonitorI2c, ReadsRawRepliesAcrossAllSizes)
{
    request.receiveFlags = 0;
    for (NvU32 size = 1; size <= MaxReceive; ++size)
    {
        Bus bus;
        bus.reply.resize(size, 0xab);
        request.transmitSize = 0;
        request.receiveSize = size;
        EXPECT_TRUE(Transfer(bus, request) == NV_OK);
        EXPECT_TRUE(bus.operations.size() == 3 && bus.operations[1].type == 'R' &&
            !bus.operations[1].more && bus.operations[1].size == size);
    }
}

TEST_F(MonitorI2c, WritesAcrossAllSizes)
{
    request.receiveFlags = 0;
    request.receiveSize = 0;
    for (NvU32 size = 1; size <= MaxTransmit; ++size)
    {
        Bus bus;
        request.transmitSize = size;
        EXPECT_TRUE(Transfer(bus, request) == NV_OK);
        EXPECT_TRUE(bus.operations.size() == 3 && bus.operations[1].type == 'W' &&
            !bus.operations[1].more);
    }
}

TEST_F(MonitorI2c, RejectsOversizedEmptyAndNullBuffers)
{
    request.receiveFlags = 0;
    request.receiveSize = 0;
    request.transmitSize = MaxTransmit + 1;
    EXPECT_TRUE(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    request.transmitSize = 0;
    request.receiveSize = MaxReceive + 1;
    EXPECT_TRUE(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    request.receiveSize = 0;
    EXPECT_TRUE(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    request.transmitSize = 1;
    request.transmit = nullptr;
    EXPECT_TRUE(Validate(request) == NV_ERR_INVALID_ARGUMENT);
}
