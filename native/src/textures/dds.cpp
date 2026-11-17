#include "textures/dds.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <utility>

namespace w2b::textures
{
    namespace
    {
        constexpr unsigned alphaPixels = 1, alpha = 2, fourCc = 4, rgb = 0x40, luminance = 0x20000;

        std::uint32_t U32(std::string_view bytes, std::size_t start)
        {
            if (start > bytes.size() || bytes.size() - start < 4)
                throw TextureError("truncated DDS header");
            std::uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[start + i])) << (i * 8);
            return value;
        }

        std::string Hex(unsigned number)
        {
            char out[16];
            const auto converted = std::to_chars(out, out + sizeof(out), number, 16);
            return "0x" + std::string(out, converted.ptr);
        }

        unsigned Bpp(Format format)
        {
            switch (format)
            {
            case Format::rgba: return 4;
            case Format::rgb: return 3;
            case Format::luminanceAlpha: return 2;
            case Format::luminance:
            case Format::alpha: return 1;
            default: return 0;
            }
        }

        unsigned BlockBytes(Format format)
        {
            switch (format)
            {
            case Format::dxt1: return 8;
            case Format::dxt3:
            case Format::dxt5:
            case Format::dxn: return 16;
            default: return 0;
            }
        }

        std::size_t Multiply(std::size_t a, std::size_t b)
        {
            if (b && a > std::numeric_limits<std::size_t>::max() / b)
                throw TextureError("DDS dimensions exceed addressable size");
            return a * b;
        }

        std::size_t Size(unsigned w, unsigned h, unsigned d, unsigned level, unsigned pixel, unsigned block)
        {
            const auto mip = [level](unsigned size) { return level >= 32 ? 1u : std::max(1u, size >> level); };
            std::size_t x = mip(w), y = mip(h);
            if (block)
            {
                x = (x + 3) / 4;
                y = (y + 3) / 4;
            }
            return Multiply(Multiply(Multiply(x, y), mip(d)), block ? block : pixel);
        }

        Format DxgiFormat(unsigned code)
        {
            constexpr std::pair<unsigned, Format> supported[] = {
                {71, Format::dxt1}, {72, Format::dxt1}, {74, Format::dxt3}, {75, Format::dxt3},
                {77, Format::dxt5}, {78, Format::dxt5}, {83, Format::dxn}, {84, Format::dxn},
                {28, Format::rgba}, {29, Format::rgba}, {65, Format::alpha}, {61, Format::luminance},
                {49, Format::luminanceAlpha}
            };
            for (const auto& [key, format] : supported)
                if (key == code)
                    return format;
            throw TextureError("unsupported DXGI format " + std::to_string(code));
        }

        Format FourCcFormat(std::string_view code)
        {
            constexpr std::pair<std::string_view, Format> supported[] = {
                {"DXT1", Format::dxt1}, {"DXT2", Format::dxt3}, {"DXT3", Format::dxt3},
                {"DXT4", Format::dxt5}, {"DXT5", Format::dxt5}, {"ATI2", Format::dxn}, {"BC5U", Format::dxn}
            };
            for (const auto& [key, format] : supported)
                if (key == code)
                    return format;
            const char quote = code.find('\'') != code.npos && code.find('"') == code.npos ? '"' : '\'';
            std::string description = "unsupported FourCC b" + std::string(1, quote);
            for (const unsigned char byte : code)
            {
                if (byte == quote || byte == '\\')
                    description += '\\';
                switch (byte)
                {
                case '\t': description += "\\t"; break;
                case '\n': description += "\\n"; break;
                case '\r': description += "\\r"; break;
                default:
                    if (byte >= 32 && byte < 127)
                        description += static_cast<char>(byte);
                    else
                    {
                        description += "\\x";
                        description += "0123456789abcdef"[byte >> 4];
                        description += "0123456789abcdef"[byte & 15];
                    }
                }
            }
            throw TextureError(description + quote);
        }

        void ReadUncompressed(DdsImage& image, std::string_view bytes, unsigned flags, unsigned bits)
        {
            const auto a = U32(bytes, 104);
            const auto r = U32(bytes, 92);
            const bool hasAlpha = (flags & (alphaPixels | alpha)) && a;
            if (flags & luminance)
            {
                if (bits == 8 && !hasAlpha)
                    image.format = Format::luminance;
                else if (bits == 16 && hasAlpha && r == 0xff && a == 0xff00)
                    image.format = Format::luminanceAlpha;
                else
                    throw TextureError("unsupported luminance layout bpp=" + std::to_string(bits));
                return;
            }
            if ((flags & alpha) && !(flags & rgb) && bits == 8)
            {
                image.format = Format::alpha;
                return;
            }
            if (!(flags & rgb) || (bits != 24 && bits != 32))
                throw TextureError("unsupported uncompressed pixel format flags=" + Hex(flags) + " bpp=" + std::to_string(bits));
            image.format = hasAlpha ? Format::rgba : Format::rgb;
            const std::array masks{r, U32(bytes, 96), U32(bytes, 100), a};
            for (unsigned channel = 0; channel < Bpp(image.format); ++channel)
            {
                const auto shift = std::countr_zero(masks[channel]);
                if (channel < 3 && (!masks[channel] || shift % 8))
                    throw TextureError("unsupported RGB masks " + Hex(masks[0]) + "/" + Hex(masks[1]) + "/" + Hex(masks[2]));
                image.channels[channel] = static_cast<unsigned>(shift / 8);
                if (image.channels[channel] >= image.sourceBytes)
                    throw TextureError("DDS channel mask exceeds pixel size");
            }
        }
    }

    unsigned FullMipCount(unsigned width, unsigned height, unsigned depth)
    {
        return std::bit_width(std::max({width, height, depth}));
    }

    std::size_t MipSize(Format format, unsigned width, unsigned height, unsigned depth, unsigned level)
    {
        return Size(width, height, depth, level, Bpp(format), BlockBytes(format));
    }

    std::size_t DdsImage::SourceMipSize(unsigned level) const
    {
        return Size(width, height, depth, level, sourceBytes, BlockBytes(format));
    }

    DdsImage ReadDds(std::string_view bytes)
    {
        if (!bytes.starts_with("DDS "))
            throw TextureError("not a DDS file");
        if (const auto size = U32(bytes, 4); size != 124)
            throw TextureError("unexpected DDS header size " + std::to_string(size));
        if (bytes.size() < 128)
            throw TextureError("truncated DDS header");
        const auto depth = std::max(1u, U32(bytes, 24));
        const auto flags = U32(bytes, 80);
        const auto bits = U32(bytes, 88);
        const auto caps = U32(bytes, 112);
        DdsImage image{Format::rgba, U32(bytes, 16), U32(bytes, 12), depth, std::max(1u, U32(bytes, 28)),
                       caps & 0x200 ? 6u : 1u, (caps & 0x200000) && depth > 1, bits / 8, {0, 1, 2, 3}, {}};
        std::size_t offset = 128;
        const auto code = bytes.substr(84, 4);
        if (!(flags & fourCc))
            ReadUncompressed(image, bytes, flags, bits);
        else if (code != "DX10")
            image.format = FourCcFormat(code);
        else
        {
            if (bytes.size() < 148)
                throw TextureError("truncated DDS DX10 header");
            offset += 20;
            image.format = DxgiFormat(U32(bytes, 128));
            image.sourceBytes = Bpp(image.format);
            if (U32(bytes, 136) & 4)
                image.faces = 6;
            if (U32(bytes, 140) > 1 && image.faces != 6)
                throw TextureError("texture arrays are not supported by IWI");
        }

        // All levels after 31 have minimum dimensions, even on a malformed
        // header advertising billions of mips. Validate their size in O(1).
        std::size_t needed = 0;
        const auto account = [&](std::size_t count) {
            if (count > std::numeric_limits<std::size_t>::max() - needed)
                throw TextureError("DDS dimensions exceed addressable size");
            needed += count;
        };
        const auto explicitMips = std::min(image.mipCount, 32u);
        for (unsigned level = 0; level < explicitMips; ++level)
            account(Multiply(image.SourceMipSize(level), image.faces));
        account(Multiply(Multiply(image.SourceMipSize(32), image.faces), image.mipCount - explicitMips));
        const auto available = bytes.size() - offset;
        if (needed > available)
            throw TextureError("truncated DDS pixel data (" + std::to_string(available) + " of " +
                               std::to_string(needed) + " bytes)");
        image.payload = bytes.substr(offset, needed);
        return image;
    }

    std::string DdsImage::ConvertPixels(std::string_view input) const
    {
        if (format != Format::rgb && format != Format::rgba)
            return std::string(input);
        const auto stride = Bpp(format);
        constexpr std::array<unsigned, 4> identity{0, 1, 2, 3};
        if (sourceBytes == stride && std::equal(channels.begin(), channels.begin() + stride, identity.begin()))
            return std::string(input);
        const auto pixels = input.size() / sourceBytes;
        std::string output(pixels * stride, '\0');
        for (std::size_t i = 0; i < pixels; ++i)
            for (unsigned channel = 0; channel < stride; ++channel)
                output[i * stride + channel] = input[i * sourceBytes + channels[channel]];
        return output;
    }
}
