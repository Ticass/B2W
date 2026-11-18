// BC endpoint selection and indices compatible with Pillow's BcnEncode.c.
// Pillow copyright and license are in native/licenses/Pillow.txt.
#include "textures/block_compress.h"

#include "core/parallel.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace w2b::textures
{
    namespace
    {
        using Block = std::array<const std::uint8_t*, 16>;
        using Color = std::array<int, 3>;

        unsigned PackColor(const std::uint8_t* pixel)
        {
            return (static_cast<unsigned>(pixel[0] >> 3) << 11) |
                   (static_cast<unsigned>(pixel[1] >> 2) << 5) | (pixel[2] >> 3);
        }

        Color UnpackColor(unsigned value)
        {
            const int red = static_cast<int>((value >> 11) << 3);
            const int green = static_cast<int>(((value >> 5) & 63) << 2);
            const int blue = static_cast<int>((value & 31) << 3);
            return {red | (red >> 5), green | (green >> 6), blue | (blue >> 5)};
        }

        void WriteIndices(char* output, std::uint64_t indices, unsigned count)
        {
            for (unsigned i = 0; i < count; ++i)
                output[i] = static_cast<char>((indices >> (8 * i)) & 255);
        }

        void ColorBlock(const Block& pixels, char* output)
        {
            const auto [min, max] = std::ranges::minmax_element(pixels, {}, PackColor);
            const auto low = PackColor(*min), high = PackColor(*max);
            WriteIndices(output, high | (low << 16), 4);
            const auto from = UnpackColor(low), to = UnpackColor(high);
            Color differences{};
            for (std::size_t c = 0; c < from.size(); ++c)
                differences[c] = std::abs(from[c] - to[c]);
            const int channels = static_cast<int>(std::ranges::count_if(differences, [](int value) { return value > 0; }));
            std::uint32_t indices = 0;
            if (channels != 0)
                for (unsigned i = 0; i < pixels.size(); ++i)
                {
                    float distance = 0;
                    for (std::size_t c = 0; c < from.size(); ++c)
                        if (differences[c] != 0)
                            distance += static_cast<float>(std::abs(pixels[i][c] - from[c])) / static_cast<float>(differences[c]);
                    distance *= static_cast<float>(6 / channels);
                    const unsigned index = distance < 1 ? 1 : distance < 3 ? 3 : distance < 5 ? 2 : 0;
                    indices |= index << (2 * i);
                }
            WriteIndices(output + 4, indices, 4);
        }

        unsigned AlphaIndex(unsigned alpha, unsigned low, unsigned high)
        {
            if (alpha == 0)
                return 6;
            if (alpha == 255)
                return 7;
            const auto distance = high == low ? 0.0f : static_cast<float>(alpha - low) / static_cast<float>(high - low) * 10.0f;
            return distance < 3 ? 2 : distance < 5 ? 3 : distance < 7 ? 4 : 5;
        }

        void AlphaBlock(const Block& pixels, char* output)
        {
            const auto [min, max] = std::ranges::minmax_element(pixels, {}, [](const std::uint8_t* pixel) { return pixel[3]; });
            const auto low = (*min)[3], high = (*max)[3];
            output[0] = static_cast<char>(low);
            output[1] = static_cast<char>(high);
            std::uint64_t indices = 0;
            for (unsigned i = 0; i < pixels.size(); ++i)
                indices |= static_cast<std::uint64_t>(AlphaIndex(pixels[i][3], low, high)) << (i * 3);
            WriteIndices(output + 2, indices, 6);
        }
    }

    IwiImage EncodeMenuTexture(std::span<const std::uint8_t> rgba, unsigned width, unsigned height, Format format)
    {
        if (format != Format::dxt1 && format != Format::dxt5)
            throw TextureError("Menu textures must use DXT1 or DXT5.");
        if (width == 0 || height == 0 || width % 4 != 0 || height % 4 != 0 ||
            rgba.size() != static_cast<std::size_t>(width) * height * 4)
            throw TextureError("Menu textures require a complete RGBA canvas sized in 4-pixel blocks.");
        const bool alpha = format == Format::dxt5;
        const unsigned blockBytes = alpha ? 16 : 8;
        const auto widthBlocks = width / 4;
        const auto heightBlocks = height / 4;
        const auto bytes = static_cast<std::size_t>(widthBlocks) * heightBlocks * blockBytes;
        if (bytes + 64 >= 16 * 1024 * 1024)
            throw TextureError("Each menu texture must be smaller than 16 MiB.");
        std::string payload(bytes, '\0');
        parallel::For(heightBlocks, [&](std::size_t row)
        {
            auto* output = payload.data() + row * widthBlocks * blockBytes;
            for (unsigned col = 0; col < widthBlocks; ++col, output += blockBytes)
            {
                Block block;
                for (unsigned y = 0; y < 4; ++y)
                    for (unsigned x = 0; x < 4; ++x)
                        block[y * 4 + x] = rgba.data() + ((4 * row + y) * width + 4 * col + x) * 4;
                if (alpha)
                    AlphaBlock(block, output);
                ColorBlock(block, output + (alpha ? 8 : 0));
            }
        }, 4);
        return BlockCompressedToIwi(width, height, format, payload);
    }
}
