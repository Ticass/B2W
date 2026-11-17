// Artwork decoding (see images.h).
#include "images.h"

#include "core/files.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <stdexcept>

#pragma comment(lib, "windowscodecs.lib")

namespace w2b::launcher
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        std::runtime_error Failure(const std::filesystem::path& path, const std::string& why)
        {
            return std::runtime_error("cannot identify image file '" + files::Utf8(path) + "': " + why);
        }

        Image DecodeTga(const std::filesystem::path& path, const std::string& data)
        {
            if (data.size() < 18)
                throw Failure(path, "truncated TGA header");
            const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
            const int idLength = bytes[0], colorMapType = bytes[1], type = bytes[2];
            const int width = bytes[12] | bytes[13] << 8, height = bytes[14] | bytes[15] << 8;
            const int depth = bytes[16], descriptor = bytes[17];
            const bool rle = type == 10 || type == 11, gray = type == 3 || type == 11;
            if (colorMapType != 0 || !(type == 2 || type == 3 || type == 10 || type == 11))
                throw Failure(path, "unsupported TGA type");
            if ((gray && depth != 8) || (!gray && depth != 24 && depth != 32) || width <= 0 || height <= 0)
                throw Failure(path, "unsupported TGA pixel depth");
            const size_t channels = static_cast<size_t>(depth / 8), pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
            std::vector<uint8_t> raw(pixels * channels);
            size_t at = 18 + static_cast<size_t>(idLength), out = 0;
            const auto need = [&](const size_t n)
            {
                if (at + n > data.size())
                    throw Failure(path, "truncated TGA pixels");
            };
            if (!rle)
            {
                need(raw.size());
                std::memcpy(raw.data(), bytes + at, raw.size());
            }
            else
                while (out < raw.size())
                {
                    need(1);
                    const uint8_t header = bytes[at++];
                    const size_t count = (header & 0x7F) + 1u;
                    if (out + count * channels > raw.size())
                        throw Failure(path, "corrupt TGA run");
                    if (header & 0x80)
                    {
                        need(channels);
                        for (size_t k = 0; k < count; k++)
                            std::memcpy(raw.data() + out + k * channels, bytes + at, channels);
                        at += channels;
                    }
                    else
                    {
                        need(count * channels);
                        std::memcpy(raw.data() + out, bytes + at, count * channels);
                        at += count * channels;
                    }
                    out += count * channels;
                }
            Image image{width, height, std::vector<uint8_t>(pixels * 4)};
            const bool topDown = (descriptor & 0x20) != 0, rightToLeft = (descriptor & 0x10) != 0;
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                {
                    const int sy = topDown ? y : height - 1 - y, sx = rightToLeft ? width - 1 - x : x;
                    const uint8_t* s = raw.data() + (static_cast<size_t>(sy) * width + sx) * channels;
                    uint8_t* d = image.rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
                    if (gray)
                        d[0] = d[1] = d[2] = s[0], d[3] = 255;
                    else
                        d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = channels == 4 ? s[3] : 255;
                }
            return image;
        }

        Image DecodeWic(const std::filesystem::path& path)
        {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool uninit = SUCCEEDED(init);
            struct Guard
            {
                bool active;
                ~Guard()
                {
                    if (active)
                        CoUninitialize();
                }
            } guard{uninit};
            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
                throw Failure(path, "imaging component unavailable");
            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
                throw Failure(path, "unsupported or unreadable image");
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))
                || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                throw Failure(path, "cannot convert the image to RGBA");
            UINT width = 0, height = 0;
            converter->GetSize(&width, &height);
            Image image{static_cast<int>(width), static_cast<int>(height), std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
            if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data())))
                throw Failure(path, "cannot read the image pixels");
            return image;
        }
    } // namespace

    uint8_t Image::MinAlpha() const
    {
        uint8_t low = 255;
        for (size_t i = 3; i < rgba.size(); i += 4)
            low = std::min(low, rgba[i]);
        return low;
    }

    Image DecodeImage(const std::filesystem::path& path)
    {
        std::string data;
        if (!files::ReadBytes(path, data))
            throw std::runtime_error("[Errno 2] No such file or directory: '" + files::Utf8(path) + "'");
        auto extension = files::Utf8(path.extension());
        for (auto& c : extension)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension == ".tga")
            return DecodeTga(path, data);
        return DecodeWic(path);
    }
} // namespace w2b::launcher
