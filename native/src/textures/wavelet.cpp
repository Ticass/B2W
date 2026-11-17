#include "textures/wavelet.h"

#include "textures/iwi.h"
#include "textures/iwi6.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace w2b::textures
{
    namespace
    {
        struct Layout
        {
            unsigned channels, stride;
            std::array<unsigned, 7> pixelFormat;
        };

        constexpr std::array<Layout, 5> layouts{{
            {4, 4, {0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000}},
            {3, 4, {0x40, 0, 32, 0xff0000, 0xff00, 0xff, 0}},
            {2, 2, {0x20001, 0, 16, 0xff, 0, 0, 0xff00}},
            {1, 1, {0x20000, 0, 8, 0xff, 0, 0, 0}},
            {1, 1, {0x2, 0, 8, 0, 0, 0, 0xff}},
        }};

        class BitReader
        {
        public:
            explicit BitReader(std::string_view bytes) : bytes_(bytes) {}

            unsigned Read(unsigned count)
            {
                const auto value = Peek(count);
                Consume(count);
                return value;
            }

            int Coefficient(const WaveletBook& table, unsigned rawBits = 9, int bias = 255)
            {
                const auto code = table[Peek(12)];
                Consume(code.bits);
                return code.value == -32768 ? static_cast<int>(Read(rawBits)) - bias : code.value;
            }

        private:
            unsigned Peek(unsigned count)
            {
                while (available_ < count && next_ < bytes_.size())
                {
                    buffer_ |= static_cast<unsigned>(static_cast<unsigned char>(bytes_[next_++])) << available_;
                    available_ += 8;
                }
                return buffer_ & ((1u << count) - 1);
            }

            void Consume(unsigned count)
            {
                if (available_ < count)
                    throw TextureError("truncated WaW wavelet bitstream");
                buffer_ >>= count;
                available_ -= count;
            }

            std::string_view bytes_;
            std::size_t next_ = 0;
            unsigned available_ = 0;
            unsigned buffer_ = 0;
        };

        struct Coefficients
        {
            int a, b, d;
        };

        Coefficients ReadCoefficients(BitReader& stream, const WaveletBook& book, unsigned bits = 9, int bias = 255)
        {
            return {stream.Coefficient(book, bits, bias), stream.Coefficient(book, bits, bias),
                    stream.Coefficient(book, bits, bias)};
        }

        char Byte(int value) { return static_cast<char>(value & 255); }

        void Haar(char* pixel, unsigned stride, unsigned rowStride, int parent,
                  unsigned parity, const Coefficients& coeff)
        {
            const auto [a, b, d] = coeff;
            const auto mean = parent * 2;
            pixel[0] = Byte(static_cast<int>(parity) + ((mean + a + b + d) >> 1));
            pixel[stride] = Byte((mean + a - b - d) >> 1);
            pixel[rowStride] = Byte((mean + b - a - d) >> 1);
            pixel[rowStride + stride] = Byte((mean + d - a - b) >> 1);
        }

        std::string DecodeFace(BitReader& stream, std::string& parent, const Layout& layout,
                               unsigned width, unsigned height, const WaveletTables& books)
        {
            const auto channels = layout.channels;
            const auto stride = layout.stride;
            const auto pixels = static_cast<std::size_t>(width) * height;
            std::string face(pixels * stride, '\0');
            if (width == 1 || height == 1)
            {
                for (std::size_t pos = 0; pos < face.size(); pos += stride)
                {
                    for (unsigned ch = 0; ch < channels; ++ch)
                        face[pos + ch] = Byte(stream.Read(8));
                    if (channels == 3)
                        face[pos + 3] = Byte(255);
                }
                return face;
            }
            if (parent.empty())
                throw TextureError("wavelet 2D level has no parent mip (NOMIPMAPS)");

            const auto& [base, delta, alpha] = books;
            if (stream.Read(1))
                for (std::size_t pos = 0; pos < parent.size(); pos += stride)
                    for (unsigned ch = 0; ch < channels; ++ch)
                        parent[pos + ch] = Byte(static_cast<unsigned char>(parent[pos + ch]) + stream.Coefficient(alpha));

            const auto rowStride = width * stride;
            for (unsigned y = 0; y < height; y += 2)
                for (unsigned x = 0; x < width; x += 2)
                {
                    auto* target = face.data() + (y * width + x) * stride;
                    const auto* coarse = parent.data() + ((y / 2) * (width / 2) + x / 2) * stride;
                    if (channels > 1)
                    {
                        const auto parity = stream.Read(1);
                        const auto blue = ReadCoefficients(stream, base);
                        Haar(target, stride, rowStride, static_cast<unsigned char>(coarse[0]), parity, blue);
                        for (unsigned ch = 1; channels >= 3 && ch < 3; ++ch)
                        {
                            const auto extraParity = stream.Read(1);
                            const auto extra = ReadCoefficients(stream, delta, 10, 510);
                            Haar(target + ch, stride, rowStride, static_cast<unsigned char>(coarse[ch]), extraParity,
                                 {blue.a + extra.a, blue.b + extra.b, blue.d + extra.d});
                        }
                    }
                    if (channels == 3)
                    {
                        target[3] = target[stride + 3] = target[rowStride + 3] = target[rowStride + stride + 3] = Byte(255);
                    }
                    else
                    {
                        const auto ch = channels - 1;
                        const auto parity = stream.Read(1);
                        const auto coeff = ReadCoefficients(stream, alpha);
                        Haar(target + ch, stride, rowStride, static_cast<unsigned char>(coarse[ch]), parity, coeff);
                    }
                }
            return face;
        }
    }

    std::string WaveletToDds(std::string_view blob, const WaveletTables& tables)
    {
        const auto header = ReadIwi6(blob);
        if (!header || !IsWaveletIwi6(blob))
            throw TextureError("not a WaW wavelet IWI6");
        if (header->flags & volumeMap || header->depth != 1)
            throw TextureError("volume wavelet IWI is not supported");
        const auto width = header->width, height = header->height;
        if (!std::has_single_bit(width) || !std::has_single_bit(height))
            throw TextureError("wavelet dimensions must be positive powers of two");
        if (static_cast<std::uint64_t>(width) * height > 16'777'216)
            throw TextureError("wavelet image exceeds 16 megapixel safety limit");

        const auto& layout = layouts[header->format - 6];
        const bool cube = (header->flags & cubemap) != 0;
        const auto faces = cube ? 6u : 1u;
        const auto count = header->flags & noMipmaps ? 1u : FullMipCount(width, height);
        std::vector<std::string> levels(count), parents(faces);
        BitReader stream(blob.substr(28));
        for (unsigned mip = count; mip-- > 0;)
        {
            const auto w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
            auto& level = levels[mip];
            level.reserve(static_cast<std::size_t>(w) * h * layout.stride * faces);
            for (auto& parent : parents)
            {
                auto face = DecodeFace(stream, parent, layout, w, h, tables);
                level += face;
                parent = std::move(face);
            }
        }
        const auto caps = 0x1000u | (count > 1 ? 0x400008u : 0) | (cube ? 8 : 0);
        auto output = PackDdsHeader({0x100fu | (count > 1 ? 0x20000u : 0), width, height, width * layout.stride,
                                     count, caps, cube ? 0xfe00u : 0, layout.pixelFormat});
        std::size_t size = output.size();
        for (const auto& level : levels)
            size += level.size();
        output.reserve(size);
        for (const auto& level : levels)
            output += level;
        return output;
    }
}
