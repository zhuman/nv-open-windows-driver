#pragma once

#include "nvkms_windows.h"
#include <cstring>

namespace nv::monitor_i2c {

constexpr NvU32 DdcCiAddress = 0x37;
constexpr NvU32 MaxTransmit = 64;
constexpr NvU32 MaxReceive = 256;
constexpr NvU32 DeviceTransmitsLength = 1;

inline NvU32 WindowsAddress(NvU32 address, bool read) noexcept
{
    // IddCx actually supplies 0x6E/0x6F despite naming the field SevenBitI2CAddress.
    return address == (read ? 0x6fU : 0x6eU) ? DdcCiAddress : address;
}

struct Request
{
    NvU32 transmitAddress;
    NvU32 transmitSize;
    const NvU8* transmit;
    NvU32 receiveAddress;
    NvU32 receiveSize;
    NvU8* receive;
    NvU32 receiveFlags;
};

inline NV_STATUS Validate(const Request& request) noexcept
{
    if ((request.transmitSize == 0 && request.receiveSize == 0) ||
        request.transmitSize > MaxTransmit ||
        request.receiveSize > MaxReceive ||
        (request.transmitSize != 0 && request.transmit == nullptr) ||
        (request.receiveSize != 0 && request.receive == nullptr) ||
        request.receiveFlags > DeviceTransmitsLength)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }
    // Only the monitor's DDC/CI endpoint is exposed, never EDID/HDCP or board I2C.
    if ((request.transmitSize != 0 && request.transmitAddress != DdcCiAddress) ||
        (request.receiveSize != 0 && request.receiveAddress != DdcCiAddress))
    {
        return NV_ERR_NOT_SUPPORTED;
    }
    if (request.receiveFlags == DeviceTransmitsLength && request.receiveSize < 3)
    {
        return NV_ERR_BUFFER_TOO_SMALL;
    }
    return NV_OK;
}

template<typename Bus>
NV_STATUS Transfer(Bus& bus, const Request& request) noexcept
{
    NV_STATUS status = Validate(request);
    if (status != NV_OK)
    {
        return status;
    }
    status = bus.Begin();
    if (status != NV_OK)
    {
        return status;
    }
    NvU8 received[MaxReceive] = {};
    if (request.transmitSize != 0)
    {
        status = bus.Write(request.transmitAddress, request.transmit,
            request.transmitSize, request.receiveSize != 0);
    }
    if (status == NV_OK && request.receiveSize != 0)
    {
        if (request.receiveFlags == DeviceTransmitsLength)
        {
            status = bus.Read(request.receiveAddress, received, 2, true);
            if (status == NV_OK)
            {
                // DDC/CI's second byte contains payload length; include checksum.
                const NvU32 total = (received[1] & 0x7fU) + 3U;
                status = total > request.receiveSize ? NV_ERR_BUFFER_TOO_SMALL :
                    bus.Read(request.receiveAddress, received + 2, total - 2, false);
            }
        }
        else
        {
            status = bus.Read(request.receiveAddress, received,
                request.receiveSize, false);
        }
    }
    const NV_STATUS finishStatus = bus.Finish();
    if (status == NV_OK)
    {
        status = finishStatus;
    }
    if (status == NV_OK && request.receiveSize != 0)
    {
        std::memcpy(request.receive, received, request.receiveSize);
    }
    return status;
}

} // namespace nv::monitor_i2c
