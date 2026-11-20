// Port of sounds.stage / translate / decode_flags / audio_name / AudioSources.
#include "stage.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/parallel.h"
#include "core/zip.h"

#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace w2b::sound
{
    namespace
    {
        using util::Json;
        namespace fs = std::filesystem;

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        std::string Backslashes(std::string text)
        {
            for (auto& c : text)
                if (c == '/')
                    c = '\\';
            return text;
        }

        // json.loads(path.read_text()) with the cp1252 locale decode
        Json ReadJson(const fs::path& path)
        {
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(path));
            return Json::parse(encoding::Cp1252ToUtf8(bytes));
        }

        bool Truthy(const Json& value)
        {
            if (value.is_null())
                return false;
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_number())
                return value.get<double>() != 0.0;
            if (value.is_string()) // json::empty() is false for every string
                return !value.get_ref<const std::string&>().empty();
            if (value.is_array() || value.is_object())
                return !value.empty();
            return true;
        }

        std::string Str(const Json& value)
        {
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_number_float())
                return util::FloatRepr(value.get<double>());
            if (value.is_boolean())
                return value.get<bool>() ? "True" : "False";
            return value.dump();
        }

        Json DecodeFlags(const Json& value)
        {
            if (!(value.is_number_integer() || value.is_number_unsigned() || value.is_boolean()))
                throw SoundError("sound flags must be an unsigned 32-bit value");
            const long long raw = value.is_boolean() ? static_cast<long long>(value.get<bool>()) : value.get<long long>();
            if (raw < 0 || raw > 0xffffffffLL)
                throw SoundError("sound flags must be an unsigned 32-bit value");
            const auto flags = static_cast<unsigned long long>(raw);
            Json out = Json::object();
            const std::pair<const char*, int> bits[] = {{"looping", 0},    {"master", 1},        {"slave", 2},       {"fullDryLevel", 3},
                                                        {"noWetLevel", 4}, {"randomLooping", 5}, {"spatialized", 6}, {"realDelay", 7},
                                                        {"distanceLpf", 8}, {"doppler", 11},     {"isBig", 12}};
            for (const auto& [name, bit] : bits)
                out[name] = static_cast<bool>(flags & (1ull << bit));
            out["maturity"] = (flags >> 9) & 3;
            out["loadType"] = (flags >> 13) & 3;
            out["legacyPriority"] = (flags >> 15) & 127;
            out["busIndex"] = (flags >> 22) & 63;
            out["moveType"] = (flags >> 28) & 7;
            out["unknownBits"] = flags & 0x80000000ull;
            return out;
        }

        const Json& Indexed(const Json& globals, const char* table, const Json& index)
        {
            const Json* hit = nullptr;
            int hits = 0;
            for (const auto& row : globals.at(table))
                if (row.at("index") == index)
                {
                    hit = &row;
                    hits++;
                }
            if (hits != 1 || !hit->contains("name") || !Truthy((*hit)["name"]))
                throw SoundError(std::string("undefined T4 ") + table + " slot " + Str(index));
            return *hit;
        }

        Json Translate(const Json& document, const Json& globals)
        {
            const auto field = [](const Json& j, const char* k) { return j.contains(k) ? j[k] : Json(); };
            if (field(document, "_game") != "T4" || field(document, "_type") != "waw2bo2_sound" || field(document, "_version") != 2)
                throw SoundError("requires sound schema 2: redump with the current T4 Unlinker");
            if (field(globals, "_game") != "T4" || field(globals, "_type") != "waw2bo2_sound_globals" || field(globals, "_version") != 1)
                throw SoundError("invalid T4 sound driver globals");
            CheckOutputName("sound", document.at("name").get<std::string>());
            Json rows = Json::array();
            for (const auto& alias : document.at("aliases"))
            {
                Json flags = DecodeFlags(alias.at("flags"));
                Json curves = Json::object();
                for (const char* key : {"volumeFalloffCurve", "volumeMinFalloffCurve", "reverbFalloffCurve", "reverbMinFalloffCurve"})
                    curves[key] = Indexed(globals, "curves", alias.at(key));
                for (const auto& [_, curve] : curves.items())
                {
                    const auto& count = curve.at("pointCount");
                    if (!(count >= 2 && count <= 8) || curve.at("points").size() != 8)
                        throw SoundError("invalid source curve " + Str(curve.at("name")));
                }
                Json references = Json::object();
                for (const char* key : {"secondaryAliasName", "chainAliasName"})
                {
                    const Json name = alias.contains(key) ? alias[key] : Json("");
                    if (Truthy(name))
                    {
                        CheckOutputName("sound", name.get<std::string>());
                        references[key] = "waw/" + name.get<std::string>();
                    }
                }
                if (alias.contains("soundFile") && !alias["soundFile"].is_null() && alias["soundFile"].at("type") != flags["loadType"])
                    throw SoundError("sound storage type disagrees with packed flags");
                Json row = Json::object();
                row["source"] = alias;
                row["flags"] = flags;
                row["references"] = references;
                row["bus"] = Indexed(globals, "buses", flags["busIndex"]);
                row["curves"] = curves;
                row["speakerMap"] = Indexed(globals, "speakerMaps", alias.at("speakerMap"));
                rows.push_back(std::move(row));
            }
            Json ir = Json::object();
            ir["_type"] = "waw2bo2_sound_ir";
            ir["_version"] = 1;
            ir["name"] = "waw/" + document.at("name").get<std::string>();
            ir["status"] = "semantic_ir_not_T6_bank";
            ir["aliases"] = std::move(rows);
            return ir;
        }

        // Path(name).suffix / with_suffix on the last component
        std::string Suffix(const std::string& name)
        {
            const auto slash = name.rfind('/');
            const auto base = slash == std::string::npos ? name : name.substr(slash + 1);
            const auto dot = base.rfind('.');
            return dot == std::string::npos || dot == 0 || dot + 1 == base.size() ? "" : base.substr(dot);
        }

        std::string WithSuffix(const std::string& name, const std::string& suffix)
        {
            const auto old = Suffix(name);
            return name.substr(0, name.size() - old.size()) + suffix;
        }

        class AudioSources
        {
        public:
            AudioSources(const std::vector<std::string>& roots, const std::vector<std::string>& iwdDirs)
                : m_roots(roots)
            {
                for (const auto& text : iwdDirs)
                {
                    const auto directory = files::FromUtf8(text);
                    std::vector<fs::path> archives;
                    std::error_code error;
                    if (fs::is_regular_file(directory, error))
                        archives.push_back(directory);
                    else if (fs::is_directory(directory, error))
                    {
                        for (const auto& item : fs::directory_iterator(directory, error))
                            if (Lower(files::Utf8(item.path().filename())).ends_with(".iwd"))
                                archives.push_back(item.path());
                        std::sort(archives.begin(), archives.end(),
                                  [](const fs::path& a, const fs::path& b) { return Lower(files::Utf8(a)) < Lower(files::Utf8(b)); });
                    }
                    for (const auto& archive : archives)
                    {
                        auto opened = std::make_shared<zip::Archive>(archive);
                        const auto label = text == files::Utf8(archive) ? text : text + "\\" + files::Utf8(archive.filename());
                        for (const auto& entry : opened->Entries())
                        {
                            if (entry.IsDir())
                                continue;
                            auto name = entry.name;
                            if (const auto nul = name.find('\0'); nul != std::string::npos)
                                name.resize(nul);
                            m_members.emplace(Lower(name), Member{opened, label, entry});
                        }
                    }
                }
            }

            std::optional<std::pair<std::string, std::string>> Read(const std::string& name, const bool loaded)
            {
                if (loaded)
                {
                    for (const auto& root : m_roots)
                        for (const auto& relative : {name, WithSuffix(name, ".xwma")})
                        {
                            const std::string path = root + "\\sound\\" + Backslashes(relative);
                            std::string data;
                            std::error_code error;
                            if (fs::is_regular_file(files::FromUtf8(path), error) && files::ReadBytes(files::FromUtf8(path), data))
                                return std::make_pair(std::move(data), path);
                        }
                }
                const auto found = m_members.find(Lower("sound/" + name));
                if (found == m_members.end())
                    return std::nullopt;
                const auto& member = found->second;
                return std::make_pair(member.archive->Read(member.entry), member.label + "!" + member.entry.name);
            }

        private:
            struct Member
            {
                std::shared_ptr<zip::Archive> archive;
                std::string label; // str(archive Path)
                zip::Entry entry;
            };
            std::vector<std::string> m_roots;
            std::unordered_map<std::string, Member> m_members; // emplace keeps the first, as setdefault
        };
    } // namespace

    Json Stage(const StageInput& input)
    {
        std::map<std::string, std::string> aliases; // name -> str(path); sorted like sorted(aliases.items())
        for (const auto& root : input.roots)
        {
            const std::string folderText = root + "\\soundaliases";
            const auto folder = files::FromUtf8(folderText);
            for (const auto& path : files::GlobSuffix(folder, ".w2bsnd.json"))
            {
                const auto generic = path.lexically_relative(folder).generic_u8string();
                std::string relative(reinterpret_cast<const char*>(generic.data()), generic.size());
                const std::string suffix = ".w2bsnd.json";
                const std::string name = relative.size() >= suffix.size() && relative.compare(relative.size() - suffix.size(), suffix.size(), suffix) == 0
                                             ? relative.substr(0, relative.size() - suffix.size())
                                             : relative;
                if (!input.names || input.names->count(name))
                    aliases.emplace(name, folderText + "\\" + Backslashes(relative));
            }
        }
        Json report = Json::object();
        report["status"] = "partial_audio_and_semantic_stage";
        report["aliases"] = Json::array();
        report["audio"] = Json::array();
        report["errors"] = Json::array();
        report["remaining"] = Json::array({"T6 driver curve/channel translation", "ducking and source-only flag translation",
                                            "original alias T6 bank compilation", "runtime playback validation"});
        if (input.names)
            for (const auto& name : *input.names)
                if (!aliases.count(name))
                    report["errors"].push_back(Json{{"alias", name}, {"reason", "missing original compiled alias"}});

        std::optional<Json> globals;
        std::error_code error;
        if (input.globalsFile && fs::is_regular_file(files::FromUtf8(*input.globalsFile), error))
            globals = ReadJson(files::FromUtf8(*input.globalsFile));
        if (globals)
        {
            files::WriteText(input.project / "sound_ir" / "driver_globals.json", util::JsonDumps(*globals, 2) + "\n");
            report["driver_globals_source"] = *input.globalsFile;
        }

        AudioSources sources(input.roots, input.iwdDirs);

        // 1. every alias on its own (parallel): IR file, report line, audio references
        struct AliasResult
        {
            Json line;    // report["aliases"] entry, or null
            Json error;   // report["errors"] entry, or null
            std::vector<std::pair<std::string, Json>> audio; // (audio_name, type) per variant with a file
        };
        std::vector<std::pair<std::string, std::string>> ordered(aliases.begin(), aliases.end());
        std::vector<AliasResult> results(ordered.size());
        parallel::For(ordered.size(),
                      [&](const size_t i)
                      {
                          const auto& [name, path] = ordered[i];
                          CheckOutputName("sound", name);
                          const Json document = ReadJson(files::FromUtf8(path));
                          auto& result = results[i];
                          try
                          {
                              if (!globals)
                                  throw SoundError("missing original T4 sound driver globals");
                              if (document.at("name") != name)
                                  throw SoundError("alias asset name disagrees with source filename");
                              const Json ir = Translate(document, *globals);
                              files::WriteText(input.project / "sound_ir" / files::FromUtf8(name + ".json"), util::JsonDumps(ir, 2) + "\n");
                              result.line = Json{{"name", ir["name"]}, {"variants", ir["aliases"].size()}, {"source", path}};
                          }
                          catch (const SoundError& e)
                          {
                              result.error = Json{{"alias", name}, {"source", path}, {"reason", e.what()}};
                          }
                          if (!document.contains("aliases"))
                              return;
                          for (const auto& alias : document["aliases"])
                              if (alias.contains("soundFile") && Truthy(alias["soundFile"]))
                                  result.audio.emplace_back(AudioName(alias["soundFile"]), alias["soundFile"].at("type"));
                      });

        // 2. merge in alias order
        std::vector<Json> items;
        std::unordered_map<std::string, size_t> index; // audio_name + "\n" + type -> item
        for (size_t i = 0; i < ordered.size(); i++)
        {
            auto& result = results[i];
            if (!result.line.is_null())
                report["aliases"].push_back(std::move(result.line));
            if (!result.error.is_null())
                report["errors"].push_back(std::move(result.error));
            for (const auto& [original, type] : result.audio)
            {
                const auto key = original + "\n" + type.dump();
                if (const auto found = index.find(key); found != index.end())
                {
                    items[found->second]["aliases"].push_back(ordered[i].first);
                    continue;
                }
                index.emplace(key, items.size());
                Json item = Json::object();
                item["name"] = original;
                item["loadType"] = type;
                item["aliases"] = Json::array({ordered[i].first});
                items.push_back(std::move(item));
            }
        }

        // 3. payloads: locate and hash in parallel, check conflicts in item order,
        //    then write each output once (two names can share one output file)
        struct Payload
        {
            bool found = false;
            bool xwma = false;
            std::string relative, source, digest;
            size_t bytes = 0;
        };
        std::vector<Payload> payloads(items.size());
        parallel::For(items.size(),
                      [&](const size_t i)
                      {
                          const auto& item = items[i];
                          const auto original = item["name"].get<std::string>();
                          const auto payload = sources.Read(original, item["loadType"] == 1);
                          if (!payload)
                              return;
                          const auto& [data, source] = *payload;
                          auto& p = payloads[i];
                          p.found = true;
                          p.xwma = data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "XWMA") == 0;
                          p.relative = "sound/waw/type_" + Str(item["loadType"]) + "/" + WithSuffix(original, p.xwma ? ".xwma" : Suffix(original));
                          p.source = source;
                          p.digest = hash::Sha256Hex(data);
                          p.bytes = data.size();
                      });
        std::unordered_map<std::string, size_t> owner; // case-folded relative -> first item writing it
        for (size_t i = 0; i < items.size(); i++)
        {
            auto& item = items[i];
            const auto& p = payloads[i];
            if (!p.found)
            {
                item["status"] = "missing_original_audio";
                continue;
            }
            const auto folded = Lower(p.relative);
            if (const auto seen = owner.find(folded); seen != owner.end() && payloads[seen->second].digest != p.digest)
                throw std::runtime_error("conflicting original audio payloads: " + Backslashes(p.relative));
            owner.emplace(folded, i);
            item["status"] = p.xwma ? "preserved_requires_decode" : "preserved_not_bank_compiled";
            item["source"] = p.source;
            item["output"] = p.relative;
            item["bytes"] = p.bytes;
            item["sha256"] = p.digest;
        }
        // Folders first, in item order: Windows keeps a folder's name as its
        // first writer spells it, and they are created in this order.
        std::vector<size_t> writers;
        std::unordered_set<std::string> folders;
        for (size_t i = 0; i < items.size(); i++)
        {
            if (!payloads[i].found)
                continue;
            const auto& relative = payloads[i].relative;
            if (folders.insert(Lower(relative.substr(0, relative.rfind('/')))).second)
                fs::create_directories((input.project / files::FromUtf8(relative)).parent_path());
            if (owner.at(Lower(payloads[i].relative)) == i)
                writers.push_back(i);
        }
        parallel::For(writers.size(),
                      [&](const size_t k)
                      {
                          const auto i = writers[k];
                          const auto payload = sources.Read(items[i]["name"].get<std::string>(), items[i]["loadType"] == 1);
                          files::WriteBytes(input.project / files::FromUtf8(payloads[i].relative), payload->first);
                      });

        for (auto& item : items)
            report["audio"].push_back(std::move(item));
        fs::create_directories(input.project);
        files::WriteText(input.project / "sounds.stage.json", util::JsonDumps(report, 2) + "\n");
        return report;
    }
} // namespace w2b::sound
