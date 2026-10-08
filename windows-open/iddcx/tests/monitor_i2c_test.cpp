#include "../../modeset/monitor_i2c.hpp"
#include "../../modeset/monitor_i2c_aux.hpp"
#include "../../modeset/monitor_i2c_native.hpp"
#include <cassert>
#include <cstdio>
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
        assert(address == DdcCiAddress);
        return Record('W', size, more);
    }
    NV_STATUS Read(NvU32 address, NvU8* data, NvU32 size, bool more) noexcept
    {
        assert(address == DdcCiAddress);
        const auto status = Record('R', size, more);
        if (status == NV_OK)
        {
            assert(readOffset + size <= reply.size());
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
        assert(parameters->displayId == 0x200 && parameters->addr == DdcCiAddress);
        assert(parameters->size < 16);
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

unsigned TestAux()
{
    unsigned cases = 0;
    NvU8 data[MaxReceive] = {};
    for (NvU32 size = 1; size <= MaxReceive; ++size)
    {
        for (NvU32 limit = 1; limit <= 16; ++limit)
        {
            for (bool read : {false, true})
            {
                AuxBackend backend;
                backend.limit = limit;
                AuxBus bus(backend, 0x200);
                assert((read ? bus.Read(DdcCiAddress, data, size, true) :
                    bus.Write(DdcCiAddress, data, size, true)) == NV_OK);
                NvU32 completed = 0;
                for (const auto& request : backend.requests)
                {
                    assert(!request.bAddrOnly && (request.cmd & 4U) != 0);
                    assert((request.cmd & 3U) == (read ? 1U : 0U));
                    assert(request.size + 1 <= size - completed);
                    completed += request.size + 1 < limit ? request.size + 1 : limit;
                }
                assert(completed == size);
                assert(bus.Finish() == NV_OK);
                const auto& stop = backend.requests.back();
                assert(stop.bAddrOnly && stop.size == 0 && stop.cmd == 0);
                const size_t before = backend.requests.size();
                assert(bus.Finish() == NV_OK && backend.requests.size() == before);
                ++cases;
            }
        }
    }
    for (NvU32 size = 1; size <= MaxReceive; ++size)
    {
        AuxBackend backend;
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, size) == NV_OK);
        assert(backend.requests.back().cmd == 1);
        for (size_t index = 0; index + 1 < backend.requests.size(); ++index)
        {
            assert(backend.requests[index].cmd == 5);
        }
        const size_t count = backend.requests.size();
        assert(bus.Finish() == NV_OK && backend.requests.size() == count);
        ++cases;
    }
    {
        AuxBackend backend;
        AuxBus bus(backend, 0x200);
        assert(bus.Write(DdcCiAddress, data, 5, true) == NV_OK);
        assert(bus.Read(DdcCiAddress, data, 11) == NV_OK);
        assert(backend.requests.size() == 2 && backend.requests[0].cmd == 4 &&
            backend.requests[1].cmd == 1);
        assert(bus.Finish() == NV_OK && backend.requests.size() == 2);
        ++cases;
    }
    {
        AuxBackend backend;
        backend.limit = 2;
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, 5) == NV_ERR_I2C_ERROR);
        assert(bus.Finish() == NV_OK && backend.requests.size() == 1);
        ++cases;
    }
    for (const NvU32 reply : {NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER})
    {
        AuxBackend backend;
        backend.replies = {{NV_OK, reply}, {NV_OK, reply}};
        AuxBus bus(backend, 0x200);
        assert(bus.Write(DdcCiAddress, data, 5) == NV_OK && backend.waits == 2);
        assert(backend.requests[0].cmd == 0);
        assert(backend.requests[1].cmd ==
            (reply == NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER ? 2U : 0U));
        assert(backend.requests[2].cmd == backend.requests[1].cmd);
        assert(bus.Finish() == NV_OK);
        ++cases;
    }
    for (const NvU32 reply : {NV0073_CTRL_DP_AUXCH_REPLYTYPE_NACK,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CNACK,
        NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT})
    {
        AuxBackend backend;
        backend.replies = {{NV_OK, reply}};
        AuxBus bus(backend, 0x200);
        const NV_STATUS expected = reply == NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT ?
            NV_ERR_TIMEOUT : NV_ERR_OBJECT_NOT_FOUND;
        assert(bus.Read(DdcCiAddress, data, 10) == expected);
        assert(bus.Finish() == NV_OK && backend.requests.back().bAddrOnly);
        ++cases;
    }
    for (NvU32 count : {0U, 17U})
    {
        AuxBackend backend;
        backend.replies = {{NV_OK, NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK, count}};
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, 16) == NV_ERR_I2C_ERROR);
        assert(bus.Finish() == NV_OK);
        ++cases;
    }
    {
        AuxBackend backend;
        backend.replies.resize(7, {NV_OK, NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER});
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, 10) == NV_ERR_TIMEOUT);
        assert(backend.requests.size() == 7 && backend.waits == 6);
        assert(bus.Finish() == NV_OK);
        ++cases;
    }
    {
        AuxBackend backend;
        backend.replies = {{NV_ERR_BUSY_RETRY, 0, ~0U, 21}};
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, 10) == NV_ERR_TIMEOUT);
        assert(backend.requests.size() == 1 && bus.Finish() == NV_OK);
        ++cases;
    }
    {
        AuxBackend backend;
        backend.replies = {{NV_ERR_INVALID_DATA}};
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, 10) == NV_ERR_INVALID_DATA);
        assert(bus.Finish() == NV_OK);
        ++cases;
    }
    {
        AuxBackend backend;
        backend.expired = true;
        AuxBus bus(backend, 0x200);
        assert(bus.Read(DdcCiAddress, data, 10) == NV_ERR_TIMEOUT);
        assert(backend.requests.empty() && bus.Finish() == NV_OK);
        ++cases;
    }
    std::printf("Monitor AUX: %u chunking, partial ACK, DEFER, write-status, STOP and error cases passed\n", cases);
    return cases;
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
        assert(parameters->address == 0x6e && parameters->flags == 0);
        assert(parameters->indexLength <= 4);
        if (!parameters->bIsWrite)
        {
            std::memset(parameters->pMessage, 0xa5, parameters->messageLength);
        }
        return status;
    }
};

void TestNative()
{
    unsigned cases = 0;
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
            assert(IndexedTransfer(backend, port, request) == NV_OK && backend.calls == 1);
            assert(backend.recorded.bIsWrite && backend.recorded.portId == port - 1 &&
                backend.recorded.messageLength == size && backend.recorded.indexLength == 0);
            ++cases;
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
                assert(IndexedTransfer(backend, port, request) == expected);
                if (prefix <= 4)
                {
                    assert(backend.calls == 1 && !backend.recorded.bIsWrite &&
                        backend.recorded.portId == port - 1 &&
                        backend.recorded.messageLength == size &&
                        backend.recorded.indexLength == prefix);
                    assert(std::memcmp(backend.recorded.index, transmit, prefix) == 0);
                    for (NvU32 index = 0; index < size; ++index) { assert(receive[index] == 0xa5); }
                    for (NvU32 index = size; index < MaxReceive; ++index) { assert(receive[index] == 0xcc); }
                }
                else { assert(backend.calls == 0 && receive[0] == 0xcc); }
                ++cases;
            }
        }
    }
    request.transmitSize = 0;
    request.receiveSize = 16;
    request.receiveFlags = 1;
    NativeBackend backend;
    assert(IndexedTransfer(backend, 1, request) == NV_ERR_NOT_SUPPORTED && backend.calls == 0);
    request.receiveFlags = 0;
    assert(IndexedTransfer(backend, 0, request) == NV_ERR_INVALID_ARGUMENT && backend.calls == 0);
    assert(IndexedTransfer(backend, 17, request) == NV_ERR_INVALID_ARGUMENT && backend.calls == 0);
    backend.status = NV_ERR_I2C_ERROR;
    std::memset(receive, 0xcc, sizeof(receive));
    assert(IndexedTransfer(backend, 1, request) == NV_ERR_I2C_ERROR && receive[0] == 0xcc);
    std::printf("Monitor native I2C: %u port, address, indexed transaction and bounds cases passed\n", cases + 4);
}

int main()
{
    NvU8 transmit[MaxTransmit] = {};
    NvU8 receive[MaxReceive] = {};
    Request request = { DdcCiAddress, 3, transmit, DdcCiAddress, MaxReceive, receive, 1 };
    unsigned cases = 0;
    for (NvU32 address = 0; address <= 255; ++address)
    {
        assert(WindowsAddress(address, false) == (address == 0x6eU ? DdcCiAddress : address));
        assert(WindowsAddress(address, true) == (address == 0x6fU ? DdcCiAddress : address));
    }
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
            assert(bus.operations.front().type == 'B');
            assert(bus.operations[1].type == 'W' && bus.operations[1].more);
            assert(bus.operations[2].type == 'R' && bus.operations[2].size == 2 &&
                bus.operations[2].more);
            assert(bus.operations.back().type == 'F');
            if (capacity < payload + 3)
            {
                assert(status == NV_ERR_BUFFER_TOO_SMALL && bus.readOffset == 2);
                for (auto byte : receive) { assert(byte == 0xcc); }
            }
            else
            {
                assert(status == NV_OK && bus.readOffset == payload + 3);
                assert(!bus.operations[3].more && bus.operations[3].size == payload + 1);
                assert(std::memcmp(receive, bus.reply.data(), payload + 3) == 0);
                for (NvU32 index = payload + 3; index < capacity; ++index)
                {
                    assert(receive[index] == 0);
                }
                for (NvU32 index = capacity; index < MaxReceive; ++index)
                {
                    assert(receive[index] == 0xcc);
                }
            }
            ++cases;
        }
    }
    request.receiveSize = 16;
    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        Bus bus;
        bus.reply = { 0x6e, 0x80, 0xbe };
        bus.failOperation = failure;
        std::memset(receive, 0xcc, sizeof(receive));
        assert(Transfer(bus, request) == NV_ERR_I2C_ERROR);
        assert(failure == 1 || bus.operations.back().type == 'F');
        for (auto byte : receive) { assert(byte == 0xcc); }
        ++cases;
    }
    {
        Bus bus;
        bus.reply = { 0x6e, 0x80, 0xbe };
        bus.finishStatus = NV_ERR_TIMEOUT;
        assert(Transfer(bus, request) == NV_ERR_TIMEOUT);
    }
    for (NvU32 address = 0; address <= 255; ++address)
    {
        const NV_STATUS expected = address == DdcCiAddress ? NV_OK : NV_ERR_NOT_SUPPORTED;
        request.transmitAddress = address;
        assert(Validate(request) == expected);
        request.transmitAddress = DdcCiAddress;
        request.receiveAddress = address;
        assert(Validate(request) == expected);
        request.receiveAddress = DdcCiAddress;
        ++cases;
    }
    for (NvU32 flags = 2; flags < 256; ++flags)
    {
        request.receiveFlags = flags;
        Bus bus;
        assert(Transfer(bus, request) == NV_ERR_INVALID_ARGUMENT && bus.operations.empty());
        ++cases;
    }
    request.receiveFlags = 0;
    for (NvU32 size = 1; size <= MaxReceive; ++size)
    {
        Bus bus;
        bus.reply.resize(size, 0xab);
        request.transmitSize = 0;
        request.receiveSize = size;
        assert(Transfer(bus, request) == NV_OK);
        assert(bus.operations.size() == 3 && bus.operations[1].type == 'R' &&
            !bus.operations[1].more && bus.operations[1].size == size);
        ++cases;
    }
    request.receiveSize = 0;
    for (NvU32 size = 1; size <= MaxTransmit; ++size)
    {
        Bus bus;
        request.transmitSize = size;
        assert(Transfer(bus, request) == NV_OK);
        assert(bus.operations.size() == 3 && bus.operations[1].type == 'W' &&
            !bus.operations[1].more);
        ++cases;
    }
    request.transmitSize = MaxTransmit + 1;
    assert(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    request.transmitSize = 0;
    request.receiveSize = MaxReceive + 1;
    assert(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    request.receiveSize = 0;
    assert(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    request.transmitSize = 1;
    request.transmit = nullptr;
    assert(Validate(request) == NV_ERR_INVALID_ARGUMENT);
    std::printf("Monitor I2C: %u framing, bounds, address, flag and cleanup cases passed\n", cases);
    TestAux();
    TestNative();
}
