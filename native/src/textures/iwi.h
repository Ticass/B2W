#pragma once

#include "textures/dds.h"

namespace w2b::textures
{
    inline constexpr unsigned noMipmaps = 2, cubemap = 4, volumeMap = 8;

    struct IwiHeader
    {
        Format format;
        unsigned flags, width, height, depth;
        std::array<std::uint32_t, 8> fileSizes{};
    };

    struct IwiImage
    {
        IwiHeader header;
        std::string bytes;
    };

    IwiImage DdsToIwi(std::string_view bytes, bool faceMajor = false, bool baseOnly = false);
    IwiImage RgbaToIwi(unsigned width, unsigned height, std::string_view pixels, bool cube = false);
    IwiImage BlockCompressedToIwi(unsigned width, unsigned height, Format format, std::string_view pixels);
}
