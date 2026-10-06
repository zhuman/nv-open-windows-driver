#pragma once

#include "display_modes.hpp"
#include "output.hpp"
#include <cstring>

namespace nv::iddcx::frame {

inline NvU16 Unorm8ToFp16(NvU8 value) noexcept
{
    if (value == 0)
    {
        return 0;
    }
    const float normalized = static_cast<float>(value) / 255.0F;
    NvU32 bits = 0;
    std::memcpy(&bits, &normalized, sizeof(bits));
    NvU32 exponent = (bits >> 23) & 0xffU;
    NvU32 mantissa = bits & 0x7fffffU;
    mantissa += 0xfffU + ((mantissa >> 13) & 1U);
    if ((mantissa & 0x800000U) != 0)
    {
        mantissa = 0;
        ++exponent;
    }
    return static_cast<NvU16>(((exponent - 112U) << 10) | (mantissa >> 13));
}

inline void CopyHdrMetadata(
    const IDDCX_HDR10_METADATA& source,
    NV_KMS_WINDOWS_HDR_METADATA* destination) noexcept
{
    const USHORT* primaries[] = {
        source.RedPrimary, source.GreenPrimary, source.BluePrimary };
    for (NvU32 primary = 0; primary < 3; ++primary)
    {
        destination->displayPrimaries[primary][0] = primaries[primary][0];
        destination->displayPrimaries[primary][1] = primaries[primary][1];
    }
    destination->whitePoint[0] = source.WhitePoint[0];
    destination->whitePoint[1] = source.WhitePoint[1];
    destination->maxMasteringLuminance = source.MaxMasteringLuminance;
    destination->minMasteringLuminance = source.MinMasteringLuminance;
    destination->maxContentLightLevel = source.MaxContentLightLevel;
    destination->maxFrameAverageLightLevel = source.MaxFrameAverageLightLevel;
}

// The caller holds Output's frame-state guard through copy and present.
inline HRESULT CopyPixels(
    Output& output, const IDDCX_SYSTEM_BUFFER_INFO& buffer, void* destinationPixels) noexcept
{
    if (destinationPixels == nullptr || output.ScanoutPitch() == 0 ||
        buffer.pBuffer == nullptr)
    {
        return E_UNEXPECTED;
    }
    const auto* mode = display_modes::FindDisplayMode(
        output.DisplayState(), output.ActiveModeIndex());
    if (mode == nullptr || buffer.Width != mode->hVisible ||
        buffer.Height != mode->vVisible)
    {
        return E_UNEXPECTED;
    }
    const bool fp16 = buffer.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool rgba = buffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        buffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = buffer.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        buffer.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool destinationFp16 =
        output.ScanoutFormat() == NV_KMS_WINDOWS_SCANOUT_FORMAT_FP16;
    if (destinationFp16 ? (!fp16 && !rgba && !bgra) :
        (!rgba && !bgra && buffer.Format != DXGI_FORMAT_B8G8R8X8_UNORM))
    {
        return DXGI_ERROR_UNSUPPORTED;
    }
    const size_t sourceBytes = static_cast<size_t>(buffer.Width) * (fp16 ? 8U : 4U);
    const size_t rowBytes = static_cast<size_t>(buffer.Width) *
        (destinationFp16 ? 8U : 4U);
    if (buffer.Pitch < sourceBytes || output.ScanoutPitch() < rowBytes)
    {
        return E_INVALIDARG;
    }
    auto* destination = static_cast<NvU8*>(destinationPixels);
    const auto* source = static_cast<const NvU8*>(buffer.pBuffer);
    for (NvU32 y = 0; y < buffer.Height; ++y)
    {
        NvU8* destinationRow = destination + static_cast<size_t>(output.ScanoutPitch()) * y;
        const NvU8* sourceRow = source + static_cast<size_t>(buffer.Pitch) * y;
        if (destinationFp16 && !fp16)
        {
            auto* halfRow = reinterpret_cast<NvU16*>(destinationRow);
            for (NvU32 x = 0; x < buffer.Width; ++x)
            {
                halfRow[x * 4] = Unorm8ToFp16(sourceRow[x * 4 + (rgba ? 0 : 2)]);
                halfRow[x * 4 + 1] = Unorm8ToFp16(sourceRow[x * 4 + 1]);
                halfRow[x * 4 + 2] = Unorm8ToFp16(sourceRow[x * 4 + (rgba ? 2 : 0)]);
                halfRow[x * 4 + 3] = Unorm8ToFp16(sourceRow[x * 4 + 3]);
            }
        }
        else if (!destinationFp16 && rgba)
        {
            for (NvU32 x = 0; x < buffer.Width; ++x)
            {
                destinationRow[x * 4] = sourceRow[x * 4 + 2];
                destinationRow[x * 4 + 1] = sourceRow[x * 4 + 1];
                destinationRow[x * 4 + 2] = sourceRow[x * 4];
                destinationRow[x * 4 + 3] = 0xff;
            }
        }
        else
        {
            std::memcpy(destinationRow, sourceRow, rowBytes);
        }
    }
    return S_OK;
}

inline HRESULT CopyPixels(Output& output, const IDDCX_SYSTEM_BUFFER_INFO& buffer) noexcept
{
    return CopyPixels(output, buffer, output.BackPixels());
}

} // namespace nv::iddcx::frame
