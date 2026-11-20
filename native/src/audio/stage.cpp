// Port of audio.stage / audio._convert_audio / audio._decode / audio.resample.
#include "stage.h"

#include "core/cache.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/parallel.h"
#include "core/process.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <map>

namespace w2b::audio
{
    namespace
    {
        using util::Json;
        namespace fs = std::filesystem;

        constexpr unsigned LOADED_RATE = 48000;

        // repr(list_of_str) as subprocess.TimeoutExpired prints the command
        std::string ReprList(const std::vector<std::string>& items)
        {
            std::string out = "[";
            for (size_t i = 0; i < items.size(); i++)
            {
                if (i)
                    out += ", ";
                const bool single = items[i].find('\'') == std::string::npos || items[i].find('"') != std::string::npos;
                const char quote = single ? '\'' : '"';
                out += quote;
                for (const char c : items[i])
                {
                    if (c == '\\' || c == quote)
                        out += '\\';
                    out += c;
                }
                out += quote;
            }
            return out + "]";
        }

        std::string Resample(const std::string_view pcm, const unsigned channels, const unsigned rate, const unsigned target, const fs::path& tool)
        {
            const std::vector<std::string> command{files::Utf8(tool), "-v", "error", "-f", "s16le", "-ar", std::to_string(rate), "-ac", std::to_string(channels),
                                                   "-i", "pipe:0", "-af", "aresample=" + std::to_string(target) + ":filter_size=128:phase_shift=14:cutoff=0.97",
                                                   "-f", "s16le", "-ar", std::to_string(target), "-ac", std::to_string(channels), "pipe:1"};
            const auto result = process::Run(command, pcm, 300);
            if (result.timedOut)
                throw SoundError("Command '" + ReprList(command) + "' timed out after 300 seconds");
            if (result.exitCode || result.out.size() % (channels * 2))
                throw SoundError("resampling failed: " + result.err.substr(0, 200));
            const auto frames = pcm.size() / (channels * 2);
            const double expected = std::nearbyint(static_cast<double>(frames) * target / rate); // round(): half to even
            if (std::abs(static_cast<double>(result.out.size() / (channels * 2)) - expected) > 2)
                throw SoundError("resampled length disagrees with the source duration");
            return result.out;
        }

        struct Decoded
        {
            std::optional<std::string> pcm;
            Json info;
            std::vector<std::string> added; // keys the resample added (listed after name/loadType)
        };

        Decoded Decode(const fs::path& source, const Tools& tools)
        {
            auto [pcm, info] = Convert(source, tools);
            std::vector<std::string> before;
            for (const auto& [key, _] : info.items())
                before.push_back(key);
            if (pcm && info["sample_rate"].get<unsigned>() != LOADED_RATE)
            {
                if (!tools.resampler)
                    throw SoundError("sound at " + std::to_string(info["sample_rate"].get<unsigned>()) + " Hz needs resampling to 48000; no resampler (FFmpeg) found");
                const auto channels = info["channels"].get<unsigned>();
                const auto data = Resample(*ParseChunks(*pcm).Find("data"), channels, info["sample_rate"].get<unsigned>(), LOADED_RATE, *tools.resampler);
                info["original_pcm_sha256"] = info["pcm_sha256"];
                info["original_frames"] = info["frames"];
                info["pcm_sha256"] = hash::Sha256Hex(data);
                info["frames"] = data.size() / (channels * 2);
                info["bank_sample_rate"] = LOADED_RATE;
                info["alias_pitch_scale"] = 1.0;
                info["sample_rate_translation"] = "resampled " + std::to_string(info["sample_rate"].get<unsigned>()) + " -> 48000 Hz (BO2 banks are 48 kHz only)";
                pcm = CanonicalPcm(data, channels, LOADED_RATE);
            }
            Decoded decoded{std::move(pcm), std::move(info), {}};
            for (const auto& [key, _] : decoded.info.items())
                if (std::find(before.begin(), before.end(), key) == before.end())
                    decoded.added.push_back(key);
            return decoded;
        }

        // The version of this file's decoding (ACM, PCM and XWMA handling, the
        // stored header). Bump it whenever decoded output can change: the
        // cache is keyed on it rather than on the program file, so rebuilds
        // and updates of the converter keep reusing the decoded audio.
        constexpr const char* DECODE_VERSION = "audio-v2";

        // audiocache: the decode is a pure function of the source bytes, this
        // decoder and the native tools
        Decoded CachedDecode(const fs::path& source, const Tools& tools)
        {
            std::string bytes;
            if (!files::ReadBytes(source, bytes))
                return Decode(source, tools); // raises the read error
            wchar_t windows[MAX_PATH];
            const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
            static const std::string codec = cache::FileIdentity(fs::path(std::wstring(windows, n)) / "SysWOW64" / "XAudio2_0.dll");
            const auto key = cache::Key({DECODE_VERSION, codec, tools.xwmaHelper ? cache::FileIdentity(*tools.xwmaHelper) : "-",
                                         tools.resampler ? cache::FileIdentity(*tools.resampler) : "-", bytes});
            if (const auto hit = cache::Load("audio_decode", key); hit && hit->size() >= 8)
            {
                uint64_t headerSize = 0;
                std::memcpy(&headerSize, hit->data(), 8);
                if (8 + headerSize <= hit->size())
                {
                    const auto header = Json::parse(hit->substr(8, static_cast<size_t>(headerSize)));
                    Decoded decoded;
                    decoded.info = header["info"];
                    for (const auto& k : header["added"])
                        decoded.added.push_back(k.get<std::string>());
                    if (header["has_pcm"].get<bool>())
                        decoded.pcm = hit->substr(static_cast<size_t>(8 + headerSize));
                    return decoded;
                }
            }
            auto decoded = Decode(source, tools);
            Json header = Json::object();
            header["info"] = decoded.info;
            header["added"] = decoded.added;
            header["has_pcm"] = decoded.pcm.has_value();
            const auto text = header.dump();
            const uint64_t headerSize = text.size();
            std::string value(reinterpret_cast<const char*>(&headerSize), 8);
            value += text;
            if (decoded.pcm)
                value += *decoded.pcm;
            cache::Store("audio_decode", key, value);
            return decoded;
        }

        // Path(relative).with_suffix(".wav")
        std::string WithWav(const std::string& relative)
        {
            const auto slash = relative.find_last_of("/\\");
            const auto dot = relative.rfind('.');
            const auto nameStart = slash == std::string::npos ? 0 : slash + 1;
            if (dot == std::string::npos || dot < nameStart || dot == nameStart)
                return relative + ".wav";
            return relative.substr(0, dot) + ".wav";
        }

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        std::string PosixPath(std::string text)
        {
            for (auto& c : text)
                if (c == '\\')
                    c = '/';
            return text;
        }

        struct Result
        {
            std::optional<std::string> pcm;
            Json info;
            std::optional<std::string> error;
        };
    } // namespace

    Json Stage(const fs::path& project, const fs::path& output, const std::string& decoderText, const Tools& tools)
    {
        std::string text;
        if (!files::ReadBytes(project / "sounds.stage.json", text))
            throw std::runtime_error("missing sounds.stage.json");
        const auto source = Json::parse(text);
        Json report = Json::object();
        report["status"] = "partial_pcm_stage";
        report["decoder"] = decoderText.empty() ? Json() : Json(decoderText);
        report["audio"] = Json::array();
        report["errors"] = Json::array();
        report["runtime_validated"] = false;

        std::vector<const Json*> tasks;
        for (const auto& entry : source["audio"])
        {
            if (!entry.contains("output"))
            {
                report["errors"].push_back(Json{{"name", entry["name"]}, {"reason", entry["status"]}});
                continue;
            }
            tasks.push_back(&entry);
        }

        std::vector<Result> results(tasks.size());
        parallel::For(tasks.size(),
                      [&](const size_t i)
                      {
                          const Json& entry = *tasks[i];
                          try
                          {
                              auto decoded = CachedDecode(project / files::FromUtf8(entry["output"].get<std::string>()), tools);
                              Json info = Json::object();
                              for (const auto& [key, value] : decoded.info.items())
                                  if (std::find(decoded.added.begin(), decoded.added.end(), key) == decoded.added.end())
                                      info[key] = value;
                              info["name"] = entry["name"];
                              info["loadType"] = entry["loadType"];
                              for (const auto& key : decoded.added)
                                  info[key] = decoded.info[key];
                              info["aliases"] = entry.contains("aliases") ? entry["aliases"] : Json::array();
                              results[i] = {std::move(decoded.pcm), std::move(info), std::nullopt};
                          }
                          catch (const SoundError& error)
                          {
                              results[i] = {std::nullopt, Json(), std::string(error.what())};
                          }
                      });

        std::map<std::string, std::string> written; // destination (case-folded, as Windows paths compare) -> pcm sha256
        for (size_t i = 0; i < tasks.size(); i++)
        {
            const Json& entry = *tasks[i];
            auto& result = results[i];
            if (result.error)
            {
                report["errors"].push_back(Json{{"name", entry["name"]}, {"reason", *result.error}});
                continue;
            }
            report["audio"].push_back(result.info);
            Json& info = report["audio"].back();
            if (!result.pcm)
                continue;
            const auto relative = WithWav(entry["output"].get<std::string>());
            const auto destination = output / files::FromUtf8(relative);
            const auto key = Lower(files::Utf8(destination.lexically_normal()));
            const auto sha = info["pcm_sha256"].get<std::string>();
            if (const auto previous = written.find(key); previous != written.end() && previous->second != sha)
            {
                report["errors"].push_back(Json{{"name", entry["name"]}, {"reason", "conflicting PCM output: " + files::Utf8(destination.lexically_normal().make_preferred())}});
                continue;
            }
            files::WriteBytes(destination, *result.pcm);
            written[key] = sha;
            info["output"] = PosixPath(relative);
        }
        fs::create_directories(output);
        files::WriteText(output / "audio.stage.json", util::JsonDumps(report, 2) + "\n");
        return report;
    }
} // namespace w2b::audio
