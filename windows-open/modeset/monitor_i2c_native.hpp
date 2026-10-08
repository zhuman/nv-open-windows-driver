#pragma once

#include "monitor_i2c.hpp"
#include <ctrl/ctrl402c.h>

namespace nv::monitor_i2c {

template<typename Backend>
NV_STATUS IndexedTransfer(Backend& backend, NvU32 port, const Request& request) noexcept
{
    const NV_STATUS validation = Validate(request);
    if (validation != NV_OK)
    {
        return validation;
    }
    if (port == 0 || port > NV402C_CTRL_NUM_I2C_PORTS)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    // This RM interface cannot stream a length-prefixed read or a >4-byte index.
    if (request.receiveFlags != 0 ||
        (request.receiveSize != 0 && request.transmitSize > NV402C_CTRL_I2C_INDEX_LENGTH_MAX))
    {
        return NV_ERR_NOT_SUPPORTED;
    }
    NV402C_CTRL_I2C_INDEXED_PARAMS parameters = {};
    parameters.portId = static_cast<NvU8>(port - 1);
    parameters.bIsWrite = request.receiveSize == 0 ? NV_TRUE : NV_FALSE;
    parameters.address = static_cast<NvU16>(
        (request.receiveSize != 0 ? request.receiveAddress : request.transmitAddress) << 1);
    NvU8 received[MaxReceive] = {};
    if (request.receiveSize != 0)
    {
        parameters.indexLength = request.transmitSize;
        if (request.transmitSize != 0)
        {
            std::memcpy(parameters.index, request.transmit, request.transmitSize);
        }
        parameters.messageLength = request.receiveSize;
        parameters.pMessage = received;
    }
    else
    {
        parameters.messageLength = request.transmitSize;
        parameters.pMessage = const_cast<NvU8*>(request.transmit);
    }
    const NV_STATUS status = backend.SubmitNative(&parameters);
    if (status == NV_OK && request.receiveSize != 0)
    {
        std::memcpy(request.receive, received, request.receiveSize);
    }
    return status;
}

} // namespace nv::monitor_i2c
