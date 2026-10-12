#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace nv::iddcx::mpo {

inline constexpr unsigned Width = 320;
inline constexpr unsigned Height = 224;
inline constexpr unsigned LutCount = 5;
inline constexpr unsigned AllFeatures = 0x3e;

class FrameCadence final
{
public:
    static constexpr std::uint64_t TicksPerSecond = 10000000;
    bool Configure(unsigned hz) noexcept
    {
        if (hz != 15 && hz != 30 && hz != 60) return false;
        interval_ = (TicksPerSecond + hz - 1) / hz;
        Reset();
        return true;
    }
    void Reset() noexcept { next_ = 0; }
    bool Due(std::uint64_t now) const noexcept { return next_ == 0 || now >= next_; }
    std::uint64_t Remaining(std::uint64_t now) const noexcept
    {
        return Due(now) ? 1 : next_ - now;
    }
    bool Submitted(std::uint64_t now) noexcept
    {
        if (interval_ == 0 || !Due(now)) return false;
        const auto delay = next_ == 0 ? interval_ : interval_ - (now - next_) % interval_;
        if (now > (std::numeric_limits<std::uint64_t>::max)() - delay) return false;
        next_ = now + delay;
        return true;
    }

private:
    std::uint64_t interval_ = 0;
    std::uint64_t next_ = 0;
};

struct Animation
{
    unsigned effect = 1;
    unsigned variant = 0;
    unsigned rotation = 0;
    bool reflection = false;
    float scale = 1;
    std::uint8_t opacity = 255;
    float gain = 1;
    unsigned lut = 0;
    float orbitX = 0;
    float orbitY = 0;
    bool pixelAlpha = false;
    bool csc = false;
};

inline Animation Animate(unsigned mode, double seconds, unsigned speed,
    unsigned plane = 0, unsigned features = AllFeatures,
    unsigned transformMask = 0xff, unsigned scaleMask = 7) noexcept
{
    const double time = seconds * static_cast<double>(speed) / 100.0 + plane * 0.7;
    Animation result;
    result.effect = mode;
    if (mode == 6)
    {
        unsigned chapters[6] = {};
        unsigned count = 0;
        for (unsigned effect = 1; effect <= 5; ++effect)
            if (features & (1U << effect)) chapters[count++] = effect;
        chapters[count++] = 6;
        result.effect = chapters[static_cast<unsigned>(time / 12.0) % count];
    }
    result.variant = static_cast<unsigned>(time / 2.0) % 5;
    result.orbitX = static_cast<float>((1 + std::sin(time * 0.43)) * 0.5);
    result.orbitY = static_cast<float>((1 + std::cos(time * 0.31)) * 0.5);
    const bool combination = result.effect == 6;
    result.pixelAlpha = result.effect == 1 || (combination && (features & (1U << 1)));
    result.csc = result.effect == 3 || (combination && (features & (1U << 3)));
    if (result.pixelAlpha)
        result.opacity = static_cast<std::uint8_t>(144 + 80 * std::sin(time * 1.3));
    if (result.effect == 2 || (combination && (features & (1U << 2))))
    {
        unsigned rotations[4] = {};
        unsigned count = 0;
        for (unsigned rotation = 0; rotation < 4; ++rotation)
            if (transformMask & ((1U << rotation) | (1U << (rotation + 4))))
                rotations[count++] = rotation;
        if (count != 0)
        {
            const unsigned rotation = rotations[static_cast<unsigned>(time / 2.0) % count];
            result.rotation = rotation * 90;
            const bool reflect = (static_cast<unsigned>(time) % 2) != 0;
            result.reflection = !(transformMask & (1U << rotation)) ||
                (reflect && (transformMask & (1U << (rotation + 4))));
        }
        constexpr float scales[] = {0.5F, 1.0F, 1.5F};
        unsigned supportedScales[3] = {};
        unsigned scaleCount = 0;
        for (unsigned scale = 0; scale < 3; ++scale)
            if (scaleMask & (1U << scale)) supportedScales[scaleCount++] = scale;
        if (scaleCount != 0)
            result.scale = scales[supportedScales[static_cast<unsigned>(time / 3.0) % scaleCount]];
        // NVKMS reserves CSC11 for scaling; combine CSC/gain at native size.
        if (combination && (features & ((1U << 3) | (1U << 4))))
            result.scale = 1;
    }
    if (result.effect == 4 || (combination && (features & (1U << 4))))
    {
        constexpr float gains[] = {0.25F, 0.5F, 1.0F, 1.5F};
        result.gain = gains[static_cast<unsigned>(time / 2.0) % 4];
    }
    if (result.effect == 5 || (combination && (features & (1U << 5))))
        result.lut = result.variant;
    return result;
}

inline float Curve(unsigned kind, float value) noexcept
{
    switch (kind)
    {
    case 1: return std::pow(value, 0.7F);
    case 2: return std::pow(value, 1.4F);
    case 3: return value * value * (3 - 2 * value);
    case 4: return std::floor(value * 7) / 7;
    default: return value;
    }
}

using Matrix = std::array<std::array<float, 4>, 3>;

inline Matrix ColorMatrix(const Animation& animation) noexcept
{
    Matrix result = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    if (animation.csc)
    {
        switch (animation.variant)
        {
        case 1:
            result = {{{0.2126F, 0.7152F, 0.0722F, 0},
                {0.2126F, 0.7152F, 0.0722F, 0}, {0.2126F, 0.7152F, 0.0722F, 0}}};
            break;
        case 2:
            result = {{{0.393F, 0.769F, 0.189F, 0},
                {0.349F, 0.686F, 0.168F, 0}, {0.272F, 0.534F, 0.131F, 0}}};
            break;
        case 3:
            result = {{{0, 1, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}}};
            break;
        case 4:
            result = {{{1.3937F, -0.3576F, -0.0361F, 0},
                {-0.1063F, 1.1424F, -0.0361F, 0}, {-0.1063F, -0.3576F, 1.4639F, 0}}};
            break;
        }
    }
    for (auto& row : result)
        for (auto& coefficient : row)
            coefficient *= animation.gain;
    return result;
}

inline bool Digit(unsigned digit, unsigned x, unsigned y) noexcept
{
    constexpr std::uint16_t glyphs[] = {
        0x7b6f, 0x2492, 0x73e7, 0x73cf, 0x5bc9,
        0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf,
    };
    return x < 3 && y < 5 && ((glyphs[digit % 10] >> (14 - (y * 3 + x))) & 1) != 0;
}

inline bool Render(std::span<std::uint8_t> pixels, unsigned pitch,
    unsigned mode, const Animation& animation, double seconds, unsigned plane) noexcept
{
    if (pitch < Width * 4 || pixels.size() < static_cast<std::size_t>(pitch) * Height)
        return false;
    const unsigned scroll = static_cast<unsigned>(seconds * 40) % Width;
    const unsigned effect = animation.effect;
    for (unsigned y = 0; y < Height; ++y)
    {
        for (unsigned x = 0; x < Width; ++x)
        {
            const float ramp = static_cast<float>((x + scroll) % Width) / (Width - 1);
            std::uint8_t r = static_cast<std::uint8_t>(255 * ramp);
            std::uint8_t g = r;
            std::uint8_t b = r;
            std::uint8_t a = 255;
            if (effect == 1 || effect == 6)
            {
                const int dx = static_cast<int>(x) - static_cast<int>(Width / 2);
                const int dy = static_cast<int>(y) - static_cast<int>(Height / 2);
                const unsigned distance = static_cast<unsigned>(std::sqrt(dx * dx + dy * dy));
                const bool checker = ((x / 24 + y / 24 + scroll / 24) & 1) != 0;
                r = checker ? 32 : 245; g = 150; b = checker ? 225 : 64;
                a = distance < 28 || distance > 150 ? 0 :
                    static_cast<std::uint8_t>((std::min)(224U, distance * 2));
                if (effect == 6)
                {
                    r = 18; g = 32; b = 58; a = 192;
                    if (x % 24 == 0 || y % 24 == 0) { g = 68; b = 96; }
                    const int wave = 112 + static_cast<int>(
                        40 * std::sin(x * 0.045 + seconds * 3.5));
                    if (std::abs(static_cast<int>(y) - wave) < 3)
                    { r = 80; g = 255; b = 180; a = 255; }
                    if (distance < 24) a = 0;
                }
            }
            else if (effect == 2)
            {
                const bool grid = x % 24 == 0 || y % 24 == 0;
                r = grid ? 220 : 24; g = grid ? 230 : 38; b = grid ? 250 : 64;
                const bool arrow = (x > 50 && x < 240 && y > 95 && y < 124) ||
                    (x >= 220 && x < 290 && std::abs(static_cast<int>(y) - 110) <
                        290 - static_cast<int>(x));
                if (arrow) { r = 250; g = 120; b = 28; }
            }
            else if (effect == 3)
            {
                constexpr std::uint8_t colors[][3] = {
                    {255, 40, 40}, {255, 220, 40}, {40, 255, 90},
                    {40, 220, 255}, {80, 70, 255}, {240, 40, 220},
                };
                const auto& color = colors[((x + scroll) * 6 / Width) % 6];
                r = color[0]; g = color[1]; b = color[2];
            }
            else if (effect == 4)
            {
                if (y > 144) { r = 255; g = static_cast<std::uint8_t>(128 + r / 2); b = 128; }
            }
            else if (effect == 5 && y > 128)
            {
                r = static_cast<std::uint8_t>(255 * ramp);
                g = static_cast<std::uint8_t>(255 * (1 - ramp));
                b = static_cast<std::uint8_t>(255 * std::abs(2 * ramp - 1));
            }
            if (x < 3 || x >= Width - 3 || y < 3 || y >= Height - 3)
            { r = 60; g = 220; b = 255; a = 255; }
            const unsigned corner = (x >= Width / 2 ? 1 : 0) + (y >= Height / 2 ? 2 : 0);
            const unsigned localX = x >= Width / 2 ? x - (Width - 26) : x - 8;
            const unsigned localY = y >= Height / 2 ? y - (Height - 38) : y - 8;
            if (Digit(corner + 1, localX / 6, localY / 6))
            { r = 255; g = 255; b = 255; a = 255; }
            if (y >= Height - 18 && y < Height - 8)
            {
                const unsigned marker = static_cast<unsigned>(seconds * 70 + plane * 30) % (Width - 64);
                if (x >= marker + 32 && x < marker + 64)
                { r = 255; g = 210; b = 32; a = 255; }
            }
            if (Digit(mode, (x - 146) / 8, (y - 12) / 8) ||
                Digit(animation.variant, (x - 186) / 8, (y - 12) / 8))
            { r = 255; g = 255; b = 255; a = 255; }
            auto* pixel = pixels.data() + static_cast<std::size_t>(y) * pitch + x * 4;
            pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = a;
        }
    }
    return true;
}

} // namespace nv::iddcx::mpo
