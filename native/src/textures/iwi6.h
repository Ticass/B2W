#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace w2b::textures
{
    struct Iwi6Header
    {
        unsigned format, flags, width, height, depth;
    };

    std::optional<Iwi6Header> ReadIwi6(std::string_view blob);
    bool IsWaveletIwi6(std::string_view blob);
    std::optional<std::string> CompressedIwi6ToDds(std::string_view blob);

    struct DdsHeader
    {
        unsigned flags, width, height, pitch, mips, caps, caps2;
        // Flags, FourCC, bit count, and RGBA masks.
        std::array<unsigned, 7> pixelFormat;
    };
    std::string PackDdsHeader(const DdsHeader& header);
}
