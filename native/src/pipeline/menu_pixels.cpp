// Filtering and rounding compatible with Pillow's Resample.c and AlphaComposite.c.
// Pillow copyright and license are in native/licenses/Pillow.txt.
#include "pipeline/menu_pixels.h"

#include "core/parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace w2b::pipeline::menu
{
    namespace
    {
        using launcher::Image;

        constexpr int FractionBits = 22;
        constexpr int One = 1 << FractionBits;

        struct Kernel
        {
            int start;
            std::vector<int> weights;
        };

        double Sinc(double value)
        {
            const auto angle = value * std::numbers::pi;
            return angle == 0 ? 1 : std::sin(angle) / angle;
        }

        double Weight(double offset, bool box)
        {
            if (box)
                return offset > -0.5 && offset <= 0.5 ? 1 : 0;
            return offset >= -3 && offset < 3 ? Sinc(offset) * Sinc(offset / 3) : 0;
        }

        std::vector<Kernel> Kernels(int from, int to, bool box)
        {
            const double scale = static_cast<double>(from) / to;
            const double inverse = 1 / std::max(scale, 1.0);
            const double support = (box ? 0.5 : 3) * std::max(scale, 1.0);
            std::vector<Kernel> result;
            result.reserve(static_cast<std::size_t>(to));
            std::vector<double> weights;
            for (int p = 0; p < to; ++p)
            {
                const double center = (p + 0.5) * scale;
                const int start = std::max(0, static_cast<int>(center - support + 0.5));
                const int stop = std::min(from, static_cast<int>(center + support + 0.5));
                weights.clear();
                double total = 0;
                for (int sample = start; sample < stop; ++sample)
                {
                    const auto value = Weight((sample - center + 0.5) * inverse, box);
                    weights.push_back(value);
                    total += value;
                }
                Kernel kernel{start, {}};
                kernel.weights.reserve(weights.size());
                for (const auto value : weights)
                    kernel.weights.push_back(total == 0 ? 0 : static_cast<int>(std::round((value / total) * One)));
                result.push_back(std::move(kernel));
            }
            return result;
        }

        template<std::size_t Channels>
        std::vector<std::uint8_t> Filter(const std::vector<std::uint8_t>& pixels, int oldWidth, int oldHeight,
                                        int newSize, bool vertical, bool box)
        {
            const int width = vertical ? oldWidth : newSize;
            const int height = vertical ? newSize : oldHeight;
            const auto kernels = Kernels(vertical ? oldHeight : oldWidth, newSize, box);
            const std::size_t step = vertical ? static_cast<std::size_t>(oldWidth) * Channels : Channels;
            std::vector<std::uint8_t> output(static_cast<std::size_t>(width) * height * Channels);
            parallel::For(static_cast<std::size_t>(height), [&](std::size_t y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const auto& kernel = kernels[vertical ? y : static_cast<std::size_t>(x)];
                    const std::size_t start = vertical ? static_cast<std::size_t>(kernel.start) * oldWidth + x :
                                                       y * oldWidth + kernel.start;
                    const auto* src = pixels.data() + start * Channels;
                    auto* dst = output.data() + (y * width + x) * Channels;
                    std::array<int, Channels> sums;
                    sums.fill(One / 2);
                    for (std::size_t sample = 0; sample < kernel.weights.size(); ++sample)
                        for (std::size_t c = 0; c < Channels; ++c)
                            sums[c] += src[sample * step + c] * kernel.weights[sample];
                    for (std::size_t c = 0; c < Channels; ++c)
                        dst[c] = static_cast<std::uint8_t>(std::clamp(sums[c] >> FractionBits, 0, 255));
                }
            }, 4);
            return output;
        }

        unsigned Divide255(unsigned value)
        {
            return (value + (value >> 8)) >> 8;
        }

        void Premultiply(std::vector<std::uint8_t>& rgba)
        {
            for (std::size_t i = 0; i < rgba.size(); i += 4)
            {
                const unsigned alpha = rgba[i + 3];
                if (alpha != 255)
                    for (unsigned c = 0; c < 3; ++c)
                        rgba[i + c] = static_cast<std::uint8_t>(Divide255(rgba[i + c] * alpha + 128));
            }
        }

        void Unpremultiply(std::vector<std::uint8_t>& rgba)
        {
            for (std::size_t i = 0; i < rgba.size(); i += 4)
            {
                const unsigned alpha = rgba[i + 3];
                if (alpha != 255 && alpha != 0)
                    for (unsigned c = 0; c < 3; ++c)
                        rgba[i + c] = static_cast<std::uint8_t>(std::min(255u, 255 * rgba[i + c] / alpha));
            }
        }
    }

    launcher::Image Resize(const Image& image, int width, int height)
    {
        if (image.width == width && image.height == height)
            return image;
        auto pixels = image.rgba;
        if (width < 1 || height < 1)
            throw std::invalid_argument("Menu texture width and height must be positive.");
        if (image.width == 0 || image.height == 0)
            return {width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        const bool alpha = image.MinAlpha() < 255;
        if (alpha)
            Premultiply(pixels);
        if (width != image.width)
            pixels = Filter<4>(pixels, image.width, image.height, width, false, false);
        if (height != image.height)
            pixels = Filter<4>(pixels, width, image.height, height, true, false);
        if (alpha)
            Unpremultiply(pixels);
        return {width, height, std::move(pixels)};
    }

    launcher::Image Crop(const Image& image, int x, int y, int width, int height)
    {
        if (width < 0 || height < 0 || x < 0 || y < 0 || x + width > image.width || y + height > image.height)
            throw std::invalid_argument("The menu art crop is outside the image.");
        Image result{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        if (width == 0 || height == 0)
            return result;
        for (int row = 0; row < height; ++row)
            std::copy_n(image.rgba.data() + (static_cast<std::size_t>(y + row) * image.width + x) * 4,
                        static_cast<std::size_t>(width) * 4, result.rgba.data() + static_cast<std::size_t>(row) * width * 4);
        return result;
    }

    void Composite(Image& destination, const Image& source, int x, int y)
    {
        parallel::For(static_cast<std::size_t>(source.height), [&](std::size_t row)
        {
            const auto* src = source.rgba.data() + row * source.width * 4;
            auto* dst = destination.rgba.data() + ((y + row) * destination.width + x) * 4;
            for (int col = 0; col < source.width; ++col, src += 4, dst += 4)
            {
                const unsigned foreground = src[3];
                if (foreground == 0)
                    continue;
                const unsigned blendedAlpha = foreground * 255 + dst[3] * (255 - foreground);
                constexpr unsigned scale = 1 << 7;
                const unsigned srcWeight = foreground * 255 * 255 * scale / blendedAlpha;
                const unsigned dstWeight = 255 * scale - srcWeight;
                for (unsigned c = 0; c < 3; ++c)
                    dst[c] = static_cast<std::uint8_t>(Divide255(src[c] * srcWeight + dst[c] * dstWeight + 128 * scale) / scale);
                dst[3] = static_cast<std::uint8_t>(Divide255(blendedAlpha + 128));
            }
        }, 4);
    }

    std::vector<std::uint8_t> ResizeAlphaBox(const Image& image, int width, int height)
    {
        std::vector<std::uint8_t> alpha;
        alpha.reserve(image.rgba.size() / 4);
        for (std::size_t i = 3; i < image.rgba.size(); i += 4)
            alpha.push_back(image.rgba[i]);
        if (width != image.width)
            alpha = Filter<1>(alpha, image.width, image.height, width, false, true);
        if (height != image.height)
            alpha = Filter<1>(alpha, width, image.height, height, true, true);
        return alpha;
    }
}
