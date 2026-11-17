#include "textures/iwi6.h"

#include "textures/iwi.h"

namespace w2b::textures
{
    namespace
    {
        unsigned Read16(std::string_view data, std::size_t offset)
        {
            return static_cast<unsigned char>(data[offset]) |
                   (static_cast<unsigned>(static_cast<unsigned char>(data[offset + 1])) << 8);
        }

        void Put32(std::string& bytes, std::size_t offset, unsigned value)
        {
            for (unsigned i = 0; i < 4; ++i)
                bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xff);
        }
    }

    std::optional<Iwi6Header> ReadIwi6(std::string_view blob)
    {
        if (blob.size() < 28 || !blob.starts_with("IWi\x06"))
            return std::nullopt;
        return Iwi6Header{static_cast<unsigned char>(blob[4]), static_cast<unsigned char>(blob[5]),
                          Read16(blob, 6), Read16(blob, 8), Read16(blob, 10)};
    }

    bool IsWaveletIwi6(std::string_view blob)
    {
        return blob.size() >= 5 && blob.starts_with("IWi\x06") && blob[4] >= 6 && blob[4] <= 10;
    }

    std::string PackDdsHeader(const DdsHeader& header)
    {
        std::string result(128, '\0');
        result.replace(0, 4, "DDS ");
        Put32(result, 4, 124);
        Put32(result, 8, header.flags);
        Put32(result, 12, header.height);
        Put32(result, 16, header.width);
        Put32(result, 20, header.pitch);
        Put32(result, 28, header.mips);
        Put32(result, 76, 32);
        for (std::size_t i = 0; i < header.pixelFormat.size(); ++i)
            Put32(result, 80 + i * 4, header.pixelFormat[i]);
        Put32(result, 108, header.caps);
        Put32(result, 112, header.caps2);
        return result;
    }

    std::optional<std::string> CompressedIwi6ToDds(std::string_view blob)
    {
        const auto header = ReadIwi6(blob);
        if (!header || (header->flags & (cubemap | volumeMap)) || header->depth != 1)
            return std::nullopt;
        unsigned fourcc;
        switch (static_cast<Format>(header->format))
        {
        case Format::dxt1: fourcc = 0x31545844; break;
        case Format::dxt3: fourcc = 0x33545844; break;
        case Format::dxt5: fourcc = 0x35545844; break;
        default: return std::nullopt;
        }
        const auto count = header->flags & noMipmaps ? 1u : FullMipCount(header->width, header->height);
        std::array<std::string_view, 16> levels{};
        std::size_t end = blob.size();
        unsigned mips = 0;
        for (; mips < count; ++mips)
        {
            const auto size = MipSize(static_cast<Format>(header->format), header->width, header->height, 1, mips);
            if (size > end - 28)
                break;
            end -= size;
            levels[mips] = blob.substr(end, size);
        }
        if (!mips)
            return std::nullopt;
        auto result = PackDdsHeader({0xa1007, header->width, header->height, static_cast<unsigned>(levels[0].size()),
                                    mips, mips > 1 ? 0x401008u : 0x1000u, 0, {4, fourcc}});
        result.reserve(128 + blob.size() - end);
        for (unsigned i = 0; i < mips; ++i)
            result.append(levels[i]);
        return result;
    }
}
