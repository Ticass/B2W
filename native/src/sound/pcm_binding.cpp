#include "sound/bindings.h"

#include "audio/convert.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/parallel.h"
#include "sound/stage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace w2b::sound
{
    namespace
    {
        namespace fs = std::filesystem;
        using py::Json;
        using Candidates = std::map<std::string, std::vector<const Json*>, std::less<>>;
        constexpr std::string_view Status = "pcm_bound_semantic_ir_not_T6_bank";

        const Json& Field(const Json& row, std::string_view key)
        {
            const auto value = row.find(key);
            if (value == row.end())
                throw SoundError(py::StringRepr(key));
            return *value;
        }

        template<typename T> T Read(std::string_view bytes, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        std::string PathText(fs::path path)
        {
            return files::Utf8(path.make_preferred());
        }

        std::string PosixPath(const fs::path& path)
        {
            auto text = files::Utf8(path);
            std::ranges::replace(text, '\\', '/');
            return text;
        }

        Candidates IndexCandidates(const Json& entries)
        {
            Candidates candidates;
            for (const auto& entry : entries)
                if (entry.contains("output"))
                    candidates[entry.at("name").get_ref<const std::string&>()].push_back(&entry);
            return candidates;
        }

        const Json& Resolve(const Candidates& candidates, const Json& file)
        {
            const auto entries = candidates.find(AudioName(file));
            const Json* hit = nullptr;
            if (entries != candidates.end())
                for (const auto* entry : entries->second)
                {
                    if (const auto storage = entry->find("loadType"); storage != entry->end() && *storage != Field(file, "type"))
                        continue;
                    if (!hit)
                    {
                        hit = entry;
                        continue;
                    }
                    for (const auto key : {"output", "pcm_sha256", "alias_pitch_scale"})
                        if (Field(*entry, key) != Field(*hit, key))
                            throw SoundError("ambiguous PCM binding for original alias variant");
                }
            if (!hit)
                throw SoundError("original audio has no successful PCM in the current manifest");
            return *hit;
        }

        class Validator
        {
        public:
            explicit Validator(const fs::path& root) : root_(root) {}

            void Check(const Json& entry)
            {
                const auto& relative = Field(entry, "output").get_ref<const std::string&>();
                CheckOutputName("audio", relative);
                const auto path = root_ / files::FromUtf8(relative);
                const auto& hash = Field(entry, "pcm_sha256").get_ref<const std::string&>();
                const auto rate = Field(entry, "bank_sample_rate").get<unsigned>();
                const auto channels = entry.value("channels", Json{});
                const auto key = std::tuple{path, hash, rate, channels};
                auto& part = cache_[std::filesystem::hash_value(path) % cache_.size()];
                const std::scoped_lock lock(part.mutex);
                if (part.verified.contains(key))
                    return;
                std::string bytes;
                if (!files::ReadBytes(path, bytes))
                    throw SoundError("[Errno 2] No such file or directory: " + py::StringRepr(PathText(path)));
                const auto chunks = audio::ViewChunks(bytes);
                const auto header = *chunks.Find("fmt ");
                if (header.size() != 16)
                    throw SoundError("PCM binding requires canonical 16-bit WAVE");
                const auto tag = Read<std::uint16_t>(header, 0);
                const auto count = Read<std::uint16_t>(header, 2);
                const auto byteRate = Read<std::uint32_t>(header, 8);
                const auto align = Read<std::uint16_t>(header, 12);
                if (chunks.form != "WAVE" || tag != 1 || Read<std::uint16_t>(header, 14) != 16 ||
                    (count != 1 && count != 2) || Read<std::uint32_t>(header, 4) != rate ||
                    (entry.contains("channels") && channels != count) || align != count * 2 || byteRate != rate * align)
                    throw SoundError("PCM header disagrees with the current manifest");
                if (hash::Sha256Hex(*chunks.Find("data")) != hash)
                    throw SoundError("stale PCM payload disagrees with the current manifest");
                part.verified.insert(key);
            }

        private:
            struct Partition
            {
                std::mutex mutex;
                std::set<std::tuple<fs::path, std::string, unsigned, Json>> verified;
            };

            const fs::path& root_;
            std::array<Partition, 64> cache_;
        };

        Json Binding(const Json& source, const Candidates& candidates, Validator& validator)
        {
            const auto file = source.find("soundFile");
            if (file == source.end() || file->is_null() || file->empty())
                throw SoundError("alias variant has no original audio file");
            const auto& entry = Resolve(candidates, *file);
            validator.Check(entry);
            const auto& scale = Field(entry, "alias_pitch_scale");
            const auto lo = Field(source, "pitchMin").get<double>() * scale.get<double>();
            const auto hi = Field(source, "pitchMax").get<double>() * scale.get<double>();
            if (!std::isfinite(lo) || !std::isfinite(hi) || lo <= 0 || hi <= 0)
                throw SoundError("invalid original or rate-compensated pitch");
            if (lo > hi)
                throw SoundError("original alias pitch range is reversed");
            const auto lower = 1200 * std::log2(lo), upper = 1200 * std::log2(hi);
            if (lower < -2400 || upper > 1200)
                throw SoundError("rate-compensated pitch exceeds native T6 bank range");
            return {{"output", entry.at("output")}, {"pcm_sha256", entry.at("pcm_sha256")},
                    {"bank_sample_rate", entry.at("bank_sample_rate")}, {"alias_pitch_scale", scale},
                    {"pitchMin", lo}, {"pitchMax", hi}, {"PitchMinCents", lower}, {"PitchMaxCents", upper}};
        }

        struct AliasResult
        {
            Json entry;
            Json errors = Json::array();
            std::size_t bound = 0;
        };

        AliasResult BindAlias(const fs::path& project, const fs::path& relative, const Candidates& candidates,
                              Validator& validator)
        {
            AliasResult result;
            auto document = Json::parse(files::ReadText(project / "sound_ir" / relative));
            std::size_t index = 0;
            const auto invalid = [&](std::string reason)
            {
                result.errors.push_back({{"alias", document.at("name")}, {"variant", index}, {"reason", std::move(reason)}});
            };
            for (auto& row : document.at("aliases"))
            {
                try
                {
                    row["pcm_binding"] = Binding(row.at("source"), candidates, validator);
                    ++result.bound;
                }
                catch (const SoundError& error) { invalid(error.what()); }
                catch (const audio::SoundError& error) { invalid(error.what()); }
                catch (const Json::type_error& error) { invalid(error.what()); }
                ++index;
            }
            document["status"] = Status;
            const auto output = fs::path("sound_bound_ir") / relative;
            files::WriteText(project / output, py::JsonDumps(document, 2) + "\n");
            result.entry = {{"name", document.at("name")}, {"output", PosixPath(output)}};
            return result;
        }

        std::wstring PathKey(const fs::path& path)
        {
            auto key = path.lexically_normal().generic_wstring();
            std::ranges::transform(key, key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return key;
        }
    }

    py::Json BindPcm(const fs::path& project, const fs::path& pcmRoot)
    {
        const auto decoded = Json::parse(files::ReadText(pcmRoot / "audio.stage.json"));
        const auto staged = Json::parse(files::ReadText(project / "sounds.stage.json"));
        const auto candidates = IndexCandidates(decoded.at("audio"));
        Validator validator(pcmRoot);
        std::vector<fs::path> paths;
        std::set<std::wstring> folders, outputs;
        bool overlapping = false;
        for (const auto& item : staged.at("aliases"))
        {
            auto name = item.at("name").get<std::string>();
            if (name.starts_with("waw/"))
                name.erase(0, 4);
            CheckOutputName("sound", name);
            auto relative = files::FromUtf8(name + ".json");
            overlapping |= !outputs.insert(PathKey(relative)).second;
            if (const auto folder = relative.parent_path(); folders.insert(PathKey(folder)).second)
                fs::create_directories(project / "sound_bound_ir" / folder);
            paths.push_back(std::move(relative));
        }

        std::vector<AliasResult> results(paths.size());
        parallel::For(paths.size(), [&](std::size_t i)
        {
            results[i] = BindAlias(project, paths[i], candidates, validator);
        }, overlapping ? 1 : 4);
        auto aliases = Json::array(), errors = Json::array();
        std::size_t bound = 0;
        for (auto& result : results)
        {
            aliases.push_back(std::move(result.entry));
            for (auto& error : result.errors)
                errors.push_back(std::move(error));
            bound += result.bound;
        }
        Json report{{"status", Status}, {"aliases", std::move(aliases)}, {"errors", std::move(errors)},
                    {"bound_variants", bound}, {"runtime_validated", false}};
        files::WriteText(project / "sounds.bindings.json", py::JsonDumps(report, 2) + "\n");
        return report;
    }
}
