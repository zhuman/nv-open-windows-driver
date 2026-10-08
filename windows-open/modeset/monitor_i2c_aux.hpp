#pragma once

#include "nvstatus.h"
#include <ctrl/ctrl0073/ctrl0073dp.h>
#include <cstring>

namespace nv::monitor_i2c {

template<typename Backend>
class AuxBus
{
public:
    AuxBus(Backend& backend, NvU32 display) noexcept : backend_(backend), display_(display) {}

    NV_STATUS Write(NvU32 address, const NvU8* data, NvU32 size, bool more = false) noexcept
    {
        return Transfer(address, const_cast<NvU8*>(data), size, false, more);
    }

    NV_STATUS Read(NvU32 address, NvU8* data, NvU32 size, bool more = false) noexcept
    {
        return Transfer(address, data, size, true, more);
    }

    NV_STATUS Finish() noexcept
    {
        if (!held_)
        {
            return NV_OK;
        }
        held_ = false;
        NV0073_CTRL_DP_AUXCH_CTRL_PARAMS parameters = {};
        parameters.displayId = display_;
        parameters.addr = address_;
        // RM supports address-only STOP as an I2C write, even after a read.
        parameters.bAddrOnly = NV_TRUE;
        parameters.cmd = NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE;
        return Command(&parameters);
    }

private:
    NV_STATUS Command(NV0073_CTRL_DP_AUXCH_CTRL_PARAMS* parameters) noexcept
    {
        const auto original = *parameters;
        NvU32 command = original.cmd;
        for (NvU32 attempt = 0; attempt < 7; ++attempt)
        {
            *parameters = original;
            parameters->cmd = command;
            const NV_STATUS status = backend_.SubmitAux(parameters);
            if (status == NV_ERR_BUSY_RETRY ||
                (status == NV_OK &&
                 (parameters->replyType == NV0073_CTRL_DP_AUXCH_REPLYTYPE_DEFER ||
                  parameters->replyType == NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER)))
            {
                if (attempt == 6 || !backend_.WaitAux(parameters->retryTimeMs))
                {
                    return NV_ERR_TIMEOUT;
                }
                if (parameters->replyType == NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CDEFER &&
                    (command & 3U) == NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE)
                {
                    command = (command & ~3U) | NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE_STATUS;
                }
                continue;
            }
            if (status != NV_OK)
            {
                return parameters->replyType == NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT ?
                    NV_ERR_TIMEOUT : status;
            }
            switch (parameters->replyType)
            {
            case NV0073_CTRL_DP_AUXCH_REPLYTYPE_ACK: return NV_OK;
            case NV0073_CTRL_DP_AUXCH_REPLYTYPE_NACK:
            case NV0073_CTRL_DP_AUXCH_REPLYTYPE_I2CNACK: return NV_ERR_OBJECT_NOT_FOUND;
            case NV0073_CTRL_DP_AUXCH_REPLYTYPE_TIMEOUT: return NV_ERR_TIMEOUT;
            default: return NV_ERR_I2C_ERROR;
            }
        }
        return NV_ERR_TIMEOUT;
    }

    NV_STATUS Transfer(NvU32 address, NvU8* data, NvU32 size, bool read, bool more) noexcept
    {
        address_ = address;
        held_ = true;
        for (NvU32 offset = 0; offset < size;)
        {
            if (backend_.AuxExpired())
            {
                return NV_ERR_TIMEOUT;
            }
            const NvU32 remaining = size - offset;
            const NvU32 count = remaining < NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE ?
                remaining : NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE;
            NV0073_CTRL_DP_AUXCH_CTRL_PARAMS parameters = {};
            parameters.displayId = display_;
            parameters.addr = address;
            const bool middle = more || count < remaining;
            parameters.cmd = (read ? NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_READ :
                NV0073_CTRL_DP_AUXCH_CMD_REQ_TYPE_WRITE) | (middle ? 4U : 0U);
            parameters.size = count - 1;
            if (!read)
            {
                std::memcpy(parameters.data, data + offset, count);
            }
            const NV_STATUS status = Command(&parameters);
            if (status != NV_OK)
            {
                return status;
            }
            held_ = middle;
            if (parameters.size == 0 || parameters.size > count)
            {
                return NV_ERR_I2C_ERROR;
            }
            // A short terminal ACK has already closed I2C; do not silently restart it.
            if (!middle && parameters.size != count)
            {
                return NV_ERR_I2C_ERROR;
            }
            if (read)
            {
                std::memcpy(data + offset, parameters.data, parameters.size);
            }
            offset += parameters.size;
        }
        return NV_OK;
    }

    Backend& backend_;
    NvU32 display_;
    NvU32 address_ = 0;
    bool held_ = false;
};

} // namespace nv::monitor_i2c
