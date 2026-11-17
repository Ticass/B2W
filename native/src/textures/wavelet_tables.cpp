#include "textures/wavelet.h"

#include "core/files.h"
#include "textures/dds.h"

#include <bit>

namespace w2b::textures
{
    namespace
    {
        std::string DecodeHex(std::string_view text)
        {
            const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            std::string result;
            for (std::size_t i = 0; i < text.size(); i += 2)
                result += static_cast<char>(digit(text[i]) * 16 + digit(text[i + 1]));
            return result;
        }

        std::int16_t ReadSigned(std::string_view data, std::size_t offset)
        {
            const auto unsignedValue = static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset]) |
                (static_cast<unsigned>(static_cast<unsigned char>(data[offset + 1])) << 8));
            return std::bit_cast<std::int16_t>(unsignedValue);
        }

        bool TableAt(std::string_view data, WaveletBook& book)
        {
            for (std::size_t i = 0; i < book.size(); ++i)
                book[i] = {ReadSigned(data, i * 4), ReadSigned(data, i * 4 + 2)};
            for (std::size_t i = 0; i < book.size(); ++i)
            {
                const auto bits = book[i].bits;
                if (bits < 1 || bits > 12 || book[i & ((1u << bits) - 1)] != book[i])
                    return false;
            }
            return true;
        }
    }

    WaveletTables LoadWaveletTables(const files::fs::path& binary)
    {
        // Only the prefix signatures are built in. Read the tables from the
        // user's AssetViewer and verify that every expanded prefix agrees.
        constexpr std::array<std::string_view, 3> signatures{
            "0c000600000003000a0006000700060004000500020005000900060001000500",
            "1e000a00020004000100030000000200040005001f000b00ffff030000000200",
            "0080040000000100010004000000010007000700000001000b00080000000100"};
        const auto path = files::fs::weakly_canonical(binary);
        const auto error = "missing/ambiguous WaW wavelet codebook in " + files::Utf8(path);
        std::string bytes;
        if (!files::ReadBytes(path, bytes))
            throw std::runtime_error("cannot read " + files::Utf8(path));

        WaveletTables tables{};
        for (std::size_t i = 0; i < signatures.size(); ++i)
        {
            const auto prefix = DecodeHex(signatures[i]);
            bool found = false;
            for (auto pos = bytes.find(prefix); pos != bytes.npos; pos = bytes.find(prefix, pos + 1))
            {
                constexpr std::size_t bookBytes = 16'384;
                WaveletBook book;
                if (bytes.size() - pos < bookBytes || !TableAt(std::string_view(bytes).substr(pos, bookBytes), book))
                    continue;
                if (found)
                    throw TextureError(error);
                tables[i] = book;
                found = true;
            }
            if (!found)
                throw TextureError(error);
        }
        return tables;
    }
}
