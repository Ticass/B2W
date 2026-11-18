// Pillow's DDS decoding (see pillow_decode.h).
#include "pillow_decode.h"

#include "dds.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace w2b::textures
{
    namespace
    {
        struct Px
        {
            std::uint8_t r, g, b, a;
        };

        Px Decode565(const std::uint16_t v)
        {
            std::uint8_t r = static_cast<std::uint8_t>((v & 0xf800) >> 8), g = static_cast<std::uint8_t>((v & 0x07e0) >> 3),
                         b = static_cast<std::uint8_t>((v & 0x001f) << 3);
            r |= r >> 5;
            g |= g >> 6;
            b |= b >> 5;
            return {r, g, b, 0xff};
        }

        // BcnDecode.c decode_bc1_color
        void Bc1Color(Px* dst, const std::uint8_t* src, const bool separateAlpha)
        {
            const std::uint16_t c0 = static_cast<std::uint16_t>(src[0] | src[1] << 8), c1 = static_cast<std::uint16_t>(src[2] | src[3] << 8);
            const std::uint32_t lut = src[4] | src[5] << 8 | src[6] << 16 | static_cast<std::uint32_t>(src[7]) << 24;
            Px p[4];
            p[0] = Decode565(c0);
            p[1] = Decode565(c1);
            const int r0 = p[0].r, g0 = p[0].g, b0 = p[0].b, r1 = p[1].r, g1 = p[1].g, b1 = p[1].b;
            if (c0 > c1 || separateAlpha)
            {
                p[2] = {static_cast<std::uint8_t>((2 * r0 + r1) / 3), static_cast<std::uint8_t>((2 * g0 + g1) / 3), static_cast<std::uint8_t>((2 * b0 + b1) / 3), 0xff};
                p[3] = {static_cast<std::uint8_t>((r0 + 2 * r1) / 3), static_cast<std::uint8_t>((g0 + 2 * g1) / 3), static_cast<std::uint8_t>((b0 + 2 * b1) / 3), 0xff};
            }
            else
            {
                p[2] = {static_cast<std::uint8_t>((r0 + r1) / 2), static_cast<std::uint8_t>((g0 + g1) / 2), static_cast<std::uint8_t>((b0 + b1) / 2), 0xff};
                p[3] = {0, 0, 0, 0};
            }
            for (int n = 0; n < 16; n++)
                dst[n] = p[3 & (lut >> (2 * n))];
        }

        // BcnDecode.c decode_bc3_alpha
        void Bc3Alpha(Px* dst, const std::uint8_t* src)
        {
            const int a0 = src[0], a1 = src[1];
            std::uint8_t a[8];
            a[0] = static_cast<std::uint8_t>(a0);
            a[1] = static_cast<std::uint8_t>(a1);
            if (a0 > a1)
                for (int i = 1; i <= 6; i++)
                    a[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
            else
            {
                for (int i = 1; i <= 4; i++)
                    a[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
                a[6] = 0;
                a[7] = 0xff;
            }
            const std::uint32_t lut1 = src[2] | src[3] << 8 | src[4] << 16, lut2 = src[5] | src[6] << 8 | src[7] << 16;
            for (int n = 0; n < 8; n++)
                dst[n].a = a[7 & (lut1 >> (3 * n))];
            for (int n = 0; n < 8; n++)
                dst[8 + n].a = a[7 & (lut2 >> (3 * n))];
        }

        void Put(RgbaImage& out, const unsigned x, const unsigned y, const Px& p)
        {
            if (x >= out.width || y >= out.height)
                return;
            auto* d = reinterpret_cast<std::uint8_t*>(out.pixels.data()) + (std::size_t(y) * out.width + x) * 4;
            d[0] = p.r, d[1] = p.g, d[2] = p.b, d[3] = p.a;
        }
    } // namespace

    RgbaImage RgbaImage::Cell(const unsigned x, const unsigned y, const unsigned w, const unsigned h, const unsigned w2, const unsigned h2) const
    {
        RgbaImage out(w2, h2);
        for (unsigned row = 0; row < h && row < h2; row++)
            for (unsigned col = 0; col < w && col < w2; col++)
            {
                // crop outside the source reads as transparent black
                if (x + col >= width || y + row >= height)
                    continue;
                std::memcpy(out.pixels.data() + (std::size_t(row) * w2 + col) * 4, pixels.data() + (std::size_t(y + row) * width + x + col) * 4, 4);
            }
        return out;
    }

    RgbaImage DecodeDdsPillow(const std::string_view bytes)
    {
        const auto image = ReadDds(bytes);
        RgbaImage out(image.width, image.height);
        const auto* src = reinterpret_cast<const std::uint8_t*>(image.payload.data());
        const auto size = image.payload.size();
        switch (image.format)
        {
        case Format::dxt1:
        case Format::dxt3:
        case Format::dxt5:
        {
            const unsigned blockBytes = image.format == Format::dxt1 ? 8 : 16;
            const unsigned bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
            if (std::size_t(bw) * bh * blockBytes > size)
                throw TextureError("truncated DDS payload");
            for (unsigned by = 0; by < bh; by++)
                for (unsigned bx = 0; bx < bw; bx++)
                {
                    const auto* block = src + (std::size_t(by) * bw + bx) * blockBytes;
                    Px px[16];
                    if (image.format == Format::dxt1)
                        Bc1Color(px, block, false);
                    else
                    {
                        Bc1Color(px, block + 8, true);
                        if (image.format == Format::dxt3)
                            for (int n = 0; n < 16; n++)
                            {
                                const int bit = n * 4;
                                const int av = 0xf & (block[bit >> 3] >> (bit & 7));
                                px[n].a = static_cast<std::uint8_t>(av << 4 | av);
                            }
                        else
                            Bc3Alpha(px, block);
                    }
                    for (int n = 0; n < 16; n++)
                        Put(out, bx * 4 + n % 4, by * 4 + n / 4, px[n]);
                }
            break;
        }
        case Format::rgba:
        case Format::rgb:
        case Format::luminance:
        case Format::luminanceAlpha:
        {
            const unsigned step = image.sourceBytes;
            if (std::size_t(image.width) * image.height * step > size)
                throw TextureError("truncated DDS payload");
            for (unsigned y = 0; y < image.height; y++)
                for (unsigned x = 0; x < image.width; x++)
                {
                    const auto* p = src + (std::size_t(y) * image.width + x) * step;
                    Px px{};
                    if (image.format == Format::luminance)
                        px = {p[0], p[0], p[0], 0xff};
                    else if (image.format == Format::luminanceAlpha)
                        px = {p[0], p[0], p[0], p[1]};
                    else
                        px = {p[image.channels[0]], p[image.channels[1]], p[image.channels[2]],
                              image.format == Format::rgba ? p[image.channels[3]] : std::uint8_t(0xff)};
                    Put(out, x, y, px);
                }
            break;
        }
        default:
            throw TextureError("DDS format Pillow does not decode here");
        }
        return out;
    }
} // namespace w2b::textures
