#include "textures/lightmap.h"

#include "core/files.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace w2b::textures
{
    namespace
    {
        struct BaseImage
        {
            unsigned width, height, stride;
            std::string pixels;

            unsigned Get(std::size_t offset, unsigned channel = 0) const
            {
                return static_cast<unsigned char>(pixels[offset * stride + (stride == 1 ? 0 : channel)]);
            }
        };

        BaseImage Load(const files::fs::path& path)
        {
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                throw std::runtime_error("could not read " + files::Utf8(path));
            const auto source = ReadDds(bytes);
            if (source.format != Format::rgba && source.format != Format::luminance)
                throw TextureError(files::Utf8(path.filename()) + ": unsupported lightmap pixel format " +
                                   std::to_string(static_cast<unsigned>(source.format)));
            if (source.faces != 1 || source.depth != 1)
                throw TextureError("lightmap requires a 2D DDS");
            return {source.width, source.height, source.format == Format::rgba ? 4u : 1u,
                    source.ConvertPixels(source.payload.substr(0, source.SourceMipSize(0)))};
        }

        std::vector<unsigned char> SunVisibility(const BaseImage& source, unsigned width, unsigned height)
        {
            if (!source.width || !source.height || source.width % width || source.height % height)
                throw TextureError("primary lightmap is not an integer multiple of the page size");
            const auto fx = source.width / width;
            const auto fy = source.height / height;
            const auto count = static_cast<std::uint64_t>(fx) * fy;
            std::vector<unsigned char> output(static_cast<std::size_t>(width) * height);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    std::uint64_t total = 0;
                    for (unsigned j = 0; j < fy; ++j)
                    {
                        const auto row = static_cast<std::size_t>(y * fy + j) * source.width;
                        for (unsigned i = 0; i < fx; ++i)
                            total += source.Get(row + x * fx + i);
                    }
                    output[static_cast<std::size_t>(y) * width + x] = static_cast<unsigned char>((total + count / 2) / count);
                }
            return output;
        }

        const std::vector<double>& FlatWeights()
        {
            static const auto weights = []
            {
                std::vector<double> table(65536);
                for (unsigned a = 0; a < 256; ++a)
                {
                    const double dx = a / 255.0 * 4.07999992 - 2.07999992;
                    for (unsigned b = 0; b < 256; ++b)
                    {
                        const double dy = b / 255.0 * 4.06451607 - 2.06451607;
                        table[a * 256 + b] = 0.6 * std::exp(-(dx * dx + dy * dy)) + 0.4;
                    }
                }
                return table;
            }();
            return weights;
        }

        unsigned RoundedByte(double value)
        {
            // Python round(x) uses nearest, with ties to even.
            const auto floor = static_cast<unsigned>(value);
            const double fraction = value - floor;
            return std::min(255u, floor + (fraction > 0.5 || (fraction == 0.5 && (floor & 1))));
        }

        void Write(char* target, std::array<unsigned, 4> color)
        {
            for (unsigned c = 0; c < color.size(); ++c)
                target[c] = static_cast<char>(color[c]);
        }
    }

    IwiImage BuildLightmap(const files::fs::path& secondary, const std::optional<files::fs::path>& primary,
                          unsigned index, bool diagnostic)
    {
        const auto source = Load(secondary);
        const auto width = source.width;
        const auto height = source.height / 2;
        if (!width || !height || source.height % 2)
            throw TextureError(files::Utf8(secondary.filename()) +
                               ": expected two equal stacked secondary lightmap layers, got " +
                               std::to_string(width) + "x" + std::to_string(source.height));
        if (width > 32767 || height > 21845)
            throw TextureError("texture dimensions do not fit an IWI header");
        const auto pixels = static_cast<std::size_t>(width) * height;
        if (pixels > (std::numeric_limits<std::uint32_t>::max() - 64u) / 24u)
            throw TextureError("lightmap payload does not fit an IWI file");
        const auto sun = primary ? SunVisibility(Load(*primary), width, height)
                                 : std::vector<unsigned char>(pixels, 255);
        std::string output(pixels * 24, '\0');
        const auto& weights = FlatWeights();
        const auto pageStride = pixels * 8;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                const auto sample = static_cast<std::size_t>(y) * width + x;
                auto* texel = output.data() + sample * 4 + static_cast<std::size_t>(y) * width * 4;
                const auto weight = weights[source.Get(sample, 3) * 256 + source.Get(sample + pixels, 3)];
                std::array<double, 3> linear;
                for (unsigned channel = 0; channel < 3; ++channel)
                {
                    const double light = (source.Get(sample, channel) + source.Get(sample + pixels, channel) * weight) / 255.0;
                    linear[channel] = light * light;
                }
                const auto peak = std::max(1.0, *std::max_element(linear.begin(), linear.end()));
                Write(texel, {RoundedByte(linear[0] / peak * 255.0), RoundedByte(linear[1] / peak * 255.0),
                              RoundedByte(linear[2] / peak * 255.0), std::max(1u, RoundedByte(255.0 / peak))});
                Write(texel + pageStride, {0, 0, 0, 255});
                Write(texel + pageStride * 2, {128, 128, 255, sun[sample]});
                texel += width * 4;
                for (unsigned channel = 0; channel < 4; ++channel)
                {
                    texel[channel] = static_cast<char>(source.Get(sample, channel));
                    texel[pageStride + channel] = static_cast<char>(source.Get(sample + pixels, channel));
                }
                Write(texel + pageStride * 2, {sun[sample], sun[sample], sun[sample], sun[sample]});
            }
        if (diagnostic)
        {
            constexpr std::array<std::array<unsigned, 4>, 5> colors{{
                {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {255, 255, 255, 255}}};
            const auto w = width * 2, h = height * 3;
            for (unsigned y = h - std::min(h, 4u); y < h; ++y)
                for (unsigned x = w - std::min(w, 4u); x < w; ++x)
                    Write(output.data() + (static_cast<std::size_t>(y) * w + x) * 4, colors[index % colors.size()]);
        }
        return RgbaToIwi(width * 2, height * 3, output);
    }
}
