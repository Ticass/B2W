#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace w2b::textures
{
    enum class Format : std::uint8_t
    {
        rgba = 1, rgb = 2, luminanceAlpha = 3, luminance = 4, alpha = 5,
        dxt1 = 11, dxt3 = 12, dxt5 = 13, dxn = 14
    };

    class TextureError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct DdsImage
    {
        Format format;
        std::uint32_t width, height, depth, mipCount, faces;
        bool volume;
        unsigned sourceBytes;
        // Source byte offsets of the output channels. Only used for RGB(A).
        std::array<unsigned, 4> channels{0, 1, 2, 3};
        std::string_view payload;

        std::size_t SourceMipSize(unsigned level) const;
        std::string ConvertPixels(std::string_view input) const;
    };

    DdsImage ReadDds(std::string_view bytes);
    std::size_t MipSize(Format format, unsigned width, unsigned height, unsigned depth, unsigned level);
    unsigned FullMipCount(unsigned width, unsigned height, unsigned depth = 1);
}
