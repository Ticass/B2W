#include "textures/iwi.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace w2b::textures
{
    namespace
    {
        void Append(std::string& bytes, unsigned value, unsigned count)
        {
            for (unsigned i = 0; i < count; ++i)
                bytes += static_cast<char>((value >> (i * 8)) & 0xff);
        }

        std::string HeaderBytes(const IwiHeader& header)
        {
            if (std::max({header.width, header.height, header.depth}) > 65535)
                throw TextureError("texture dimensions do not fit an IWI header");
            std::string bytes = "IWi\x1b";
            bytes += static_cast<char>(header.format);
            bytes += static_cast<char>(header.flags);
            Append(bytes, header.width, 2);
            Append(bytes, header.height, 2);
            Append(bytes, header.depth, 2);
            bytes.append(20, '\0'); // gamma and maxGlossForMip
            for (auto size : header.fileSizes)
                Append(bytes, size, 4);
            return bytes;
        }

        Format OutputFormat(Format format)
        {
            switch (format)
            {
            case Format::rgb:
            case Format::luminanceAlpha:
            case Format::luminance: return Format::rgba;
            default: return format;
            }
        }

        std::string Expand(std::string_view level, Format format)
        {
            const unsigned stride = format == Format::rgb ? 3 : format == Format::luminanceAlpha ? 2 : 1;
            std::string output(level.size() / stride * 4, '\xff');
            for (std::size_t source = 0, dest = 0; source < level.size(); source += stride, dest += 4)
            {
                output[dest] = level[source];
                output[dest + 1] = level[source + (stride == 3 ? 1 : 0)];
                output[dest + 2] = level[source + (stride == 3 ? 2 : 0)];
                if (stride == 2)
                    output[dest + 3] = level[source + 1];
            }
            return output;
        }

        std::string ConvertLevel(const DdsImage& source, std::size_t offset, std::size_t faceSize, bool faceMajor)
        {
            if (!faceMajor || source.faces == 1)
                return source.ConvertPixels(source.payload.substr(offset * source.faces, faceSize * source.faces));
            std::string level;
            level.reserve(faceSize * source.faces);
            const auto faceStride = source.payload.size() / source.faces;
            for (unsigned face = 0; face < source.faces; ++face)
                level += source.payload.substr(offset + face * faceStride, faceSize);
            return source.ConvertPixels(level);
        }

        std::uint32_t FileSize(std::size_t payloadSize)
        {
            if (payloadSize > std::numeric_limits<std::uint32_t>::max() - 64)
                throw TextureError("texture payload does not fit an IWI file");
            return static_cast<std::uint32_t>(payloadSize + 64);
        }
    }

    IwiImage DdsToIwi(std::string_view bytes, bool faceMajor, bool baseOnly)
    {
        const auto image = ReadDds(bytes);
        const auto depth = image.volume ? image.depth : 1;
        const auto full = FullMipCount(image.width, image.height, depth);
        const bool complete = image.mipCount >= full && !baseOnly;
        IwiHeader header{OutputFormat(image.format),
                         (complete ? 0u : noMipmaps) | (image.faces == 6 ? cubemap : 0u) |
                         (image.volume ? volumeMap : 0u),
                         image.width, image.height, depth};
        std::vector<std::string> levels;
        std::size_t offset = 0;
        for (unsigned level = 0; level < (complete ? full : 1); ++level)
        {
            const auto size = image.SourceMipSize(level);
            auto pixels = ConvertLevel(image, offset, size, faceMajor);
            offset += size;
            if (header.format != image.format)
                pixels = Expand(pixels, image.format);
            const auto expected = MipSize(header.format, header.width, header.height, depth, level) * image.faces;
            if (pixels.size() != expected)
                throw TextureError("mip " + std::to_string(level) + ": " + std::to_string(pixels.size()) +
                                   " bytes, expected " + std::to_string(expected));
            levels.push_back(std::move(pixels));
        }

        std::size_t total = 0;
        for (std::size_t i = levels.size(); i-- > 0;)
        {
            total += levels[i].size();
            const auto size = FileSize(total);
            if (i < header.fileSizes.size())
                header.fileSizes[i] = size;
        }
        auto out = HeaderBytes(header);
        out.reserve(FileSize(total));
        for (auto level = levels.rbegin(); level != levels.rend(); ++level)
            out += *level;
        return {header, std::move(out)};
    }

    IwiImage BlockCompressedToIwi(unsigned width, unsigned height, Format format, std::string_view pixels)
    {
        if (pixels.size() != MipSize(format, width, height, 1, 0))
            throw TextureError("compressed data size does not match the image dimensions");
        IwiHeader header{format, noMipmaps, width, height, 1, {FileSize(pixels.size())}};
        auto bytes = HeaderBytes(header);
        bytes += pixels;
        return {header, std::move(bytes)};
    }

    IwiImage RgbaToIwi(unsigned width, unsigned height, std::string_view pixels)
    {
        if (pixels.size() != MipSize(Format::rgba, width, height, 1, 0))
            throw TextureError("RGBA data size does not match the image dimensions");
        IwiHeader header{Format::rgba, noMipmaps, width, height, 1, {FileSize(pixels.size())}};
        auto bytes = HeaderBytes(header);
        bytes += pixels;
        return {header, std::move(bytes)};
    }
}
