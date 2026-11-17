#pragma once
// Artwork decoding for the launcher: PNG, JPEG, BMP, GIF and TIFF through the
// Windows Imaging Component, TGA (uncompressed and RLE) here, since WIC has no
// TGA codec. Pixels are straight-alpha RGBA, top row first.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace w2b::launcher
{
    struct Image
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        uint8_t MinAlpha() const;
    };

    // throws std::runtime_error with a message for the user
    Image DecodeImage(const std::filesystem::path& path);
} // namespace w2b::launcher
