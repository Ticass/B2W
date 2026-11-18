// Port of audio.convert / audio.chunks / acm.decode / xwma.decode.
#include "convert.h"

#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"

#include <windows.h>
#include <mmsystem.h> // FOURCC, which WIN32_LEAN_AND_MEAN leaves out and msacm.h needs
#include <mmreg.h>
#include <msacm.h>

#include <cmath>
#include <cstring>
#include <mutex>

#pragma comment(lib, "msacm32.lib")

namespace w2b::audio
{
    namespace
    {
        using py::Json;
        namespace fs = std::filesystem;

        constexpr unsigned T6_RATES[] = {8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000};
        constexpr const char* SUPPORTED_DLL_SHA256 = "e435b73193bdf651f7ae564eba05266595ac672db45e0e22dce92d0bcb3c6513";

        template<typename T> T Read(const std::string_view data, const size_t at)
        {
            T value{};
            std::memcpy(&value, data.data() + at, sizeof(T));
            return value;
        }

        std::string Hex(const std::string_view data)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(data.size() * 2);
            for (const auto c : data)
            {
                out += digits[static_cast<unsigned char>(c) >> 4];
                out += digits[static_cast<unsigned char>(c) & 0xF];
            }
            return out;
        }

        // bytes.decode("ascii", errors="replace")
        std::string Ascii(const std::string_view tag)
        {
            std::string out;
            for (const auto c : tag)
                out += static_cast<unsigned char>(c) < 0x80 ? std::string(1, c) : std::string("\xEF\xBF\xBD");
            return out;
        }

        std::string AcmDecode(const std::string& fmt, const std::string& data)
        {
            if (fmt.size() < 18)
                throw SoundError("ACM requires complete WAVEFORMATEX");
            const auto channels = Read<uint16_t>(fmt, 2);
            const auto rate = Read<uint32_t>(fmt, 4);
            WAVEFORMATEX target{WAVE_FORMAT_PCM, channels, rate, rate * channels * 2u, static_cast<WORD>(channels * 2), 16, 0};
            std::string source = fmt + std::string(1, '\0'); // ctypes.create_string_buffer adds a NUL
            // ACM failed concurrent opens now and then (MMRESULT 512, ACMERR_NOTPOSSIBLE,
            // on a sound it decodes alone): one conversion at a time
            static std::mutex acm;
            const std::lock_guard serialized(acm);
            HACMSTREAM handle = nullptr;
            const auto check = [](const MMRESULT result, const char* operation)
            {
                if (result)
                    throw SoundError(std::string("Windows ACM ") + operation + " failed: MMRESULT " + std::to_string(result));
            };
            check(acmStreamOpen(&handle, nullptr, reinterpret_cast<LPWAVEFORMATEX>(source.data()), &target, nullptr, 0, 0, ACM_STREAMOPENF_NONREALTIME),
                  "open");
            ACMSTREAMHEADER header{};
            bool prepared = false;
            std::string input = data;
            std::string output;
            try
            {
                DWORD size = 0;
                check(acmStreamSize(handle, static_cast<DWORD>(data.size()), &size, ACM_STREAMSIZEF_SOURCE), "size");
                if (!size)
                    throw SoundError("Windows ACM returned zero destination size");
                output.assign(size, '\0');
                header.cbStruct = sizeof(header);
                header.pbSrc = reinterpret_cast<LPBYTE>(input.data());
                header.cbSrcLength = static_cast<DWORD>(input.size());
                header.pbDst = reinterpret_cast<LPBYTE>(output.data());
                header.cbDstLength = size;
                check(acmStreamPrepareHeader(handle, &header, 0), "prepare");
                prepared = true;
                check(acmStreamConvert(handle, &header, ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END), "convert");
                if (header.cbSrcLengthUsed != data.size() || header.cbDstLengthUsed > size)
                    throw SoundError("Windows ACM did not consume the complete source");
                output.resize(header.cbDstLengthUsed);
            }
            catch (...)
            {
                if (prepared)
                    acmStreamUnprepareHeader(handle, &header, 0);
                acmStreamClose(handle, 0);
                throw;
            }
            acmStreamUnprepareHeader(handle, &header, 0);
            acmStreamClose(handle, 0);
            return output;
        }

        // xwma.validate
        std::pair<uint32_t, unsigned> ValidateXwma(const Chunks& chunks)
        {
            const std::string& fmt = *chunks.Find("fmt ");
            const std::string* found = chunks.Find("dpds");
            const std::string sizes = found ? *found : std::string();
            const std::string& data = *chunks.Find("data");
            if (fmt.size() < 18)
                throw SoundError("truncated XWMA format");
            const auto tag = Read<uint16_t>(fmt, 0);
            const auto channels = Read<uint16_t>(fmt, 2);
            const auto rate = Read<uint32_t>(fmt, 4);
            const auto packet = Read<uint16_t>(fmt, 12);
            const auto extra = Read<uint16_t>(fmt, 16);
            if ((tag != 0x161 && tag != 0x162) || (channels != 1 && channels != 2) || !rate || !packet || extra)
                throw SoundError("XWMA profile requires native implementation/verification");
            if (sizes.empty() || sizes.size() % 4 || data.size() % packet || sizes.size() / 4 != data.size() / packet)
                throw SoundError("inconsistent XWMA packet/decoded-size table");
            std::vector<uint32_t> values(sizes.size() / 4);
            std::memcpy(values.data(), sizes.data(), sizes.size());
            for (size_t i = 1; i < values.size(); i++)
                if (values[i - 1] > values[i])
                    throw SoundError("invalid XWMA decoded packet sizes");
            if (values.back() % (channels * 2u))
                throw SoundError("invalid XWMA decoded packet sizes");
            if (values.back() > 0x40000000u)
                throw SoundError("XWMA decoded payload exceeds native bridge limit");
            return {values.back(), channels};
        }

        fs::path SystemCodec()
        {
            wchar_t buffer[MAX_PATH];
            const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
            const fs::path root(std::wstring(buffer, length));
            std::error_code error;
            return root / (fs::is_directory(root / "SysWOW64", error) ? "SysWOW64" : "System32") / "XAudio2_0.dll";
        }

        std::string CodecDigest(const fs::path& dll)
        {
            static std::mutex lock;
            static std::string path, digest;
            const std::lock_guard guard(lock);
            const auto identity = files::Utf8(dll);
            if (path != identity)
            {
                std::string bytes;
                files::ReadBytes(dll, bytes);
                digest = hash::Sha256Hex(bytes);
                path = identity;
            }
            return digest;
        }

        std::pair<std::string, Json> XwmaDecode(const fs::path& source, const Chunks& chunks, const Tools& tools)
        {
            const auto [expected, channels] = ValidateXwma(chunks);
            if (!tools.xwmaHelper)
                throw SoundError("native XWMA bridge not built: build tools/xaudio_wma_decoder.cpp (x86) or use --xwma-decoder");
            const auto dll = SystemCodec();
            std::error_code error;
            if (!fs::is_regular_file(dll, error))
                throw SoundError("original XAudio2 2.0 codec is missing from Windows");
            const auto digest = CodecDigest(dll);
            if (digest != SUPPORTED_DLL_SHA256)
                throw SoundError("installed XAudio2 2.0 version is not audited; refusing unverified internal codec ABI");

            wchar_t temp[MAX_PATH];
            GetTempPathW(MAX_PATH, temp);
            wchar_t file[MAX_PATH];
            GetTempFileNameW(temp, L"wxw", 0, file);
            const fs::path destination(file);
            const auto result = process::Run({files::Utf8(*tools.xwmaHelper), files::Utf8(fs::weakly_canonical(source)), files::Utf8(destination)}, "", 30);
            std::string pcm;
            const bool written = files::ReadBytes(destination, pcm) && fs::file_size(destination, error) > 0;
            fs::remove(destination, error);
            if (result.timedOut)
                throw SoundError("native XWMA decoding timed out");
            if (result.exitCode || !written)
                throw SoundError("native XWMA bridge failed (" + std::to_string(result.exitCode) + "): " + result.err.substr(0, 1000));
            Json info;
            try
            {
                info = Json::parse(result.out);
            }
            catch (const std::exception&)
            {
                throw SoundError("invalid native XWMA decoder report");
            }
            const auto number = [&](const char* key) -> long long
            {
                const auto it = info.find(key);
                return it != info.end() && it->is_number() ? it->get<long long>() : -1;
            };
            if (pcm.size() != expected || number("decoded_bytes") != expected || number("dpds_bytes") != expected || number("input_remaining") != 0)
                throw SoundError("native XWMA sample count disagrees with original packet metadata");
            Json native = Json::object();
            native["decoder"] = "installed XAudio2_0 xWMA";
            native["decoder_dll_sha256"] = digest;
            native["expected_pcm_bytes"] = expected;
            native["decoded_pcm_bytes"] = pcm.size();
            const auto callbacks = info.find("callbacks");
            native["native_input_callbacks"] = callbacks != info.end() ? *callbacks : Json();
            native["channels"] = channels;
            return {pcm, native};
        }
    } // namespace

    const std::string* Chunks::Find(const std::string_view tag) const
    {
        for (const auto& [t, bytes] : parts)
            if (t == tag)
                return &bytes;
        return nullptr;
    }

    const std::string_view* ChunkViews::Find(std::string_view tag) const
    {
        for (const auto& [name, bytes] : parts)
            if (name == tag)
                return &bytes;
        return nullptr;
    }

    ChunkViews ViewChunks(std::string_view data)
    {
        if (data.size() < 12 || data.compare(0, 4, "RIFF") != 0 || (data.compare(8, 4, "WAVE") != 0 && data.compare(8, 4, "XWMA") != 0))
            throw SoundError("unsupported audio container; expected original RIFF WAVE/XWMA");
        const uint64_t riffEnd = static_cast<uint64_t>(Read<uint32_t>(data, 4)) + 8;
        if (riffEnd < 12 || riffEnd > data.size())
            throw SoundError("RIFF byte count exceeds or invalidates the original container");
        ChunkViews chunks{data.substr(8, 4), {}};
        uint64_t offset = 12;
        while (offset < riffEnd)
        {
            if (offset + 8 > riffEnd)
                throw SoundError("truncated RIFF chunk header");
            const auto tag = data.substr(static_cast<size_t>(offset), 4);
            const auto size = Read<uint32_t>(data, static_cast<size_t>(offset) + 4);
            const uint64_t end = offset + 8 + size;
            if (end > riffEnd || chunks.Find(tag))
                throw SoundError("truncated or duplicate RIFF chunk");
            chunks.parts.emplace_back(tag, data.substr(static_cast<size_t>(offset) + 8, size));
            offset = end + (size & 1);
        }
        if (!chunks.Find("fmt ") || !chunks.Find("data"))
            throw SoundError("missing RIFF format/audio data");
        return chunks;
    }

    Chunks ParseChunks(const std::string& data)
    {
        const auto views = ViewChunks(data);
        Chunks chunks{std::string(views.form), {}};
        chunks.parts.reserve(views.parts.size());
        for (const auto& [tag, bytes] : views.parts)
            chunks.parts.emplace_back(tag, bytes);
        return chunks;
    }

    std::string CanonicalPcm(const std::string_view pcm, const unsigned channels, const unsigned rate)
    {
        if ((channels != 1 && channels != 2) || std::find(std::begin(T6_RATES), std::end(T6_RATES), rate) == std::end(T6_RATES))
            throw SoundError("T6 bank cannot preserve channels=" + std::to_string(channels) + ", sample rate=" + std::to_string(rate));
        if (pcm.size() % (channels * 2))
            throw SoundError("partial PCM frame");
        std::string out = "RIFF";
        const auto put32 = [&](const uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
        const auto put16 = [&](const uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); };
        put32(static_cast<uint32_t>(36 + pcm.size()));
        out += "WAVEfmt ";
        put32(16);
        put16(1);
        put16(static_cast<uint16_t>(channels));
        put32(rate);
        put32(rate * channels * 2);
        put16(static_cast<uint16_t>(channels * 2));
        put16(16);
        out += "data";
        put32(static_cast<uint32_t>(pcm.size()));
        out += pcm;
        return out;
    }

    std::pair<std::optional<std::string>, Json> Convert(const fs::path& source, const Tools& tools)
    {
        std::string original;
        if (!files::ReadBytes(source, original))
            throw std::runtime_error("cannot read " + files::Utf8(source));
        const auto chunks = ParseChunks(original);
        const std::string& fmt = *chunks.Find("fmt ");
        if (fmt.size() < 16)
            throw SoundError("truncated audio format");
        const auto tag = Read<uint16_t>(fmt, 0);
        const auto channels = Read<uint16_t>(fmt, 2);
        const auto rate = Read<uint32_t>(fmt, 4);
        const auto align = Read<uint16_t>(fmt, 12);
        const auto bits = Read<uint16_t>(fmt, 14);
        Json report = Json::object();
        report["source_sha256"] = hash::Sha256Hex(original);
        report["format"] = tag;
        report["channels"] = channels;
        report["sample_rate"] = rate;
        Json sizes = Json::object();
        for (const auto& [t, bytes] : chunks.parts)
            sizes[Ascii(t)] = bytes.size();
        report["source_chunks"] = sizes;
        const uint64_t riffEnd = static_cast<uint64_t>(Read<uint32_t>(original, 4)) + 8;
        if (riffEnd < original.size())
        {
            report["riff_trailing_bytes"] = original.size() - riffEnd;
            report["riff_trailing_sha256"] = hash::Sha256Hex(std::string_view(original).substr(static_cast<size_t>(riffEnd)));
        }
        if (rate <= 0)
            throw SoundError("invalid source sample rate");
        unsigned target = T6_RATES[0];
        double best = std::abs(std::log(static_cast<double>(T6_RATES[0]) / rate));
        for (const auto r : T6_RATES)
        {
            const double distance = std::abs(std::log(static_cast<double>(r) / rate));
            if (distance < best)
            {
                best = distance;
                target = r;
            }
        }
        CanonicalPcm("", channels, target);
        report["bank_sample_rate"] = target;
        report["alias_pitch_scale"] = static_cast<double>(rate) / target;
        std::string pcm;
        const std::string& data = *chunks.Find("data");
        if (chunks.form == "WAVE" && tag == 1 && bits == 16 && align == channels * 2)
        {
            pcm = data;
            report["status"] = "exact_original_pcm";
        }
        else if (chunks.form == "WAVE" && tag == 2 && bits == 4)
        {
            if (fmt.size() < 50 || align < 7u * channels || data.size() % align)
                throw SoundError("incomplete Microsoft ADPCM blocks");
            const auto samplesPerBlock = Read<uint16_t>(fmt, 18);
            if (samplesPerBlock != 2 + (static_cast<int>(align) - 7 * channels) * 2 / channels)
                throw SoundError("inconsistent Microsoft ADPCM samples per block");
            static constexpr int16_t coefficients[] = {256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232};
            if (Read<uint16_t>(fmt, 20) != 7 || std::memcmp(fmt.data() + 22, coefficients, sizeof(coefficients)) != 0)
                throw SoundError("custom ADPCM coefficient table requires XAudio2 semantics verification");
            pcm = AcmDecode(fmt, data);
            const auto expected = data.size() / align * samplesPerBlock * channels * 2;
            if (pcm.size() != expected)
                throw SoundError("native ADPCM output length disagrees with source blocks");
            report["status"] = "native_ms_adpcm";
            report["decoder"] = "Windows ACM msadp32";
            report["sample_reference"] = "XAudio2_0 native decoder agreement (audited standard 7-coefficient format)";
            report["expected_pcm_bytes"] = expected;
            report["decoded_pcm_bytes"] = pcm.size();
            report["adpcm_samples_per_block"] = samplesPerBlock;
        }
        else if (chunks.form == "XWMA" && (tag == 0x161 || tag == 0x162))
        {
            auto [decoded, native] = XwmaDecode(source, chunks, tools);
            pcm = std::move(decoded);
            for (const auto& [key, value] : native.items())
                report[key] = value;
            report["status"] = "native_xwma";
        }
        else
        {
            char text[64]; // f"{tag:#x}" prints 0x0 for zero, unlike printf's %#x
            std::snprintf(text, sizeof(text), "audio codec 0x%x/%u bits requires implementation", tag, bits);
            throw SoundError(text);
        }
        auto wav = CanonicalPcm(pcm, channels, target);
        if (rate != target)
            report["sample_rate_translation"] = "original PCM unchanged; multiply both alias pitches by alias_pitch_scale";
        report["frames"] = pcm.size() / (channels * 2u);
        report["pcm_sha256"] = hash::Sha256Hex(pcm);
        if (const auto* priv = chunks.Find("PRIV"))
            report["original_private_metadata_hex"] = Hex(*priv);
        if (chunks.Find("smpl") || chunks.Find("cue "))
        {
            report["status"] = "loop_region_requires_translation";
            Json loop = Json::object();
            for (const char* t : {"smpl", "cue "})
                if (const auto* bytes = chunks.Find(t))
                    loop[t] = Hex(*bytes);
            report["loop_metadata"] = loop;
            return {std::nullopt, report};
        }
        return {std::move(wav), report};
    }
} // namespace w2b::audio
