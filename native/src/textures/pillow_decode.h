#pragma once
// Image.open(dds).convert("RGBA") as Pillow decodes it: the base level of a
// DXT1/3/5, RGB(A), L or LA DDS, with BcnDecode.c's integer palettes.
// Pillow copyright and license are in native/licenses/Pillow.txt.

#include <string>
#include <string_view>

namespace w2b::textures
{
    struct RgbaImage
    {
        unsigned width = 0, height = 0;
        std::string pixels; // width * height * 4, rows top to bottom

        RgbaImage() = default;
        RgbaImage(unsigned w, unsigned h) : width(w), height(h), pixels(std::size_t(w) * h * 4, '\0') {}
        // Image.crop((x, y, x + w, y + h)) pasted at (0, 0) of a cleared w2 x h2 image
        RgbaImage Cell(unsigned x, unsigned y, unsigned w, unsigned h, unsigned w2, unsigned h2) const;
    };

    RgbaImage DecodeDdsPillow(std::string_view bytes);
}
