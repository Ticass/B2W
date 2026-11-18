// Localized UI text (see localization.h).
#include "localization.h"
#include "extract/unlinker.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/parallel.h"
#include "core/process.h"
#include "core/pyre.h"
#include "core/zip.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "gsc/tokens.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <set>

namespace w2b::strings
{
    namespace fs = std::filesystem;
    using py::Json;

    namespace
    {
        const std::string PREFIX = "WAW2BO2_";
        // wawhud.ENGINE_STRINGS: T4's engine-owned scoreboard and ammo labels
        const std::vector<std::string> ENGINE_STRINGS{"CGAME_SB_POINTS", "CGAME_SB_KILLS", "CGAME_SB_DOWNS", "CGAME_SB_REVIVES", "CGAME_SB_HEADSHOTS",
                                                      "CGAME_SB_PING", "PLATFORM_RELOAD", "PLATFORM_LOW_AMMO_NO_RELOAD", "WEAPON_NO_AMMO"};

        using Ordered = std::vector<std::pair<std::string, std::string>>;

        std::string Str(fs::path p)
        {
            return files::Utf8(p.make_preferred());
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        bool IsDir(const fs::path& p)
        {
            std::error_code e;
            return fs::is_directory(p, e);
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        std::string Upper(std::string s)
        {
            for (auto& c : s)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return s;
        }

        bool StartsWith(std::string_view s, std::string_view p)
        {
            return s.substr(0, p.size()) == p;
        }

        // KEY.fullmatch: [A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]+
        bool IsKey(const std::string& s)
        {
            static const pyre::Regex key("[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]+\\Z");
            return key.FullMatch(s).has_value();
        }

        bool Identifier(const std::string& s)
        {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        }

        std::string ReadBytes(const fs::path& p)
        {
            std::string data;
            if (!files::ReadBytes(p, data))
                throw std::runtime_error("cannot read " + Str(p));
            return data;
        }

        // read_text(encoding="utf-8-sig", errors="replace")
        std::string ReadSigReplace(const fs::path& p)
        {
            return encoding::UniversalNewlines(encoding::Utf8SigReplace(ReadBytes(p)));
        }

        Json ReadJson(const fs::path& p)
        {
            return Json::parse(files::ReadText(p));
        }

        // sorted(Path list): case-insensitive with backslashes on Windows
        void SortPaths(std::vector<fs::path>& paths)
        {
            std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
        }

        // root.glob("<language>/localizedstrings/**/*.str") | root.glob("localizedstrings/**/*.str"), sorted
        std::vector<fs::path> SourceFiles(const fs::path& root, const std::string& language)
        {
            std::vector<fs::path> found;
            std::set<std::string> seen;
            for (const auto& folder : {root / files::FromUtf8(language) / "localizedstrings", root / "localizedstrings"})
                for (const auto& p : files::RGlob(folder, ".str"))
                    if (seen.insert(Lower(Str(p))).second)
                        found.push_back(p);
            SortPaths(found);
            return found;
        }

        // dump_zone: a localize-only dump in its own cache folder
        fs::path DumpZone(const fs::path& ff, const fs::path& unlinker, const fs::path& work, const std::vector<fs::path>& search)
        {
            const auto out = work / files::FromUtf8(files::Utf8(ff.stem()) + "_" + hash::Sha256Hex(Str(extract::Resolve(ff))).substr(0, 12));
            const auto marker = out / "localization.cache.json";
            const auto ffStamp = extract::Stamp(ff), toolStamp = extract::Stamp(unlinker);
            const Json identity{{"ff_size", ffStamp[0]}, {"ff_mtime", ffStamp[1]}, {"tool_mtime", toolStamp[1]}, {"version", 1}};
            if (IsFile(marker) && py::SortKeys(ReadJson(marker)) == py::SortKeys(identity))
                return out;
            std::error_code e;
            fs::remove_all(out, e);
            fs::create_directories(out);
            std::string joined;
            for (size_t i = 0; i < search.size(); i++)
                joined += (i ? ";" : "") + Str(search[i]);
            const auto result = extract::RunUnlinker({Str(unlinker), "--no-color", "--include-assets", "localize", "--search-path", joined, "--output-folder", Str(out), Str(ff)});
            files::WriteBytes(out / "extract.log", result.out);
            if (result.exitCode)
                throw std::runtime_error("localization extraction failed: " + Str(ff) + "; see " + Str(out / "extract.log"));
            files::WriteText(marker, py::JsonDumpsDefault(identity));
            return out;
        }

        // price_variant: an authored stock hint family extended to the map's price
        std::optional<std::pair<std::string, std::string>> PriceVariant(const std::string& key, const std::map<std::string, std::pair<std::string, std::string>>& sources)
        {
            static const pyre::Regex family("(ZOMBIE_BUTTON_BUY_(?:OPEN_DOOR|OPEN_AREA|CLEAR_DEBRIS)_)([0-9]+)");
            const auto match = family.FullMatch(key);
            if (!match)
                return std::nullopt;
            const auto prefix = (*match)[1], cost = (*match)[2];
            Ordered templates; // insertion order
            for (const auto& [candidate, hit] : sources)
            {
                if (!StartsWith(candidate, prefix))
                    continue;
                const auto oldCost = candidate.substr(prefix.size());
                if (oldCost.empty() || !std::all_of(oldCost.begin(), oldCost.end(), [](const char c) { return c >= '0' && c <= '9'; }))
                    continue;
                const pyre::Regex pattern("(?<![0-9])" + pyre::Escape(oldCost) + "(?![0-9])");
                if (pattern.FindAll(hit.first).size() != 1)
                    continue;
                const auto templ = pattern.Sub([](const pyre::Match&) { return std::string("{waw_cost}"); }, hit.first);
                if (std::none_of(templates.begin(), templates.end(), [&](const auto& t) { return t.first == templ; }))
                    templates.emplace_back(templ, hit.second);
            }
            if (templates.size() != 1)
                return std::nullopt;
            auto value = templates[0].first;
            const auto at = value.find("{waw_cost}");
            for (size_t pos = at; pos != std::string::npos; pos = value.find("{waw_cost}", pos + cost.size()))
                value.replace(pos, 10, cost);
            return std::pair{value, templates[0].second};
        }

        std::string StrRepr(const fs::path& p)
        {
            return Str(p);
        }
    } // namespace

    std::string Quote(const std::string& text)
    {
        std::string out = "\"";
        for (const char c : text)
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
            }
        return out + "\"";
    }

    std::string Unquote(const std::string& text)
    {
        if (!StartsWith(text, "\""))
            return text;
        // text[1:-1] with \\ \" \n \r \t escapes resolved
        const auto body = text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string();
        std::string out;
        for (size_t i = 0; i < body.size(); i++)
        {
            if (body[i] == '\\' && i + 1 < body.size() && std::string_view("\\\"nrt").find(body[i + 1]) != std::string_view::npos)
            {
                const char c = body[++i];
                out += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
            }
            else
                out += body[i];
        }
        return out;
    }

    namespace
    {
        // parse(): ordered key -> value, conflicts as Python lists them
        Ordered Parse(const std::string& text, const std::string& language, Json* conflicts)
        {
            static const pyre::Regex word("//[^\\n]*|/\\*.*?\\*/|\"(?:\\\\.|[^\"\\\\])*\"|[^\\s]+", pyre::DOTALL);
            // newline counts by code point offset
            std::vector<int> lines;
            lines.reserve(text.size() + 1);
            int count = 0;
            for (size_t i = 0; i < text.size(); i++)
            {
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                    lines.push_back(count);
                if (text[i] == '\n')
                    count++;
            }
            lines.push_back(count);
            std::vector<std::pair<std::string, long long>> tokens;
            for (const auto& m : word.FindIter(text))
            {
                const auto t = m[0];
                if (!StartsWith(t, "//") && !StartsWith(t, "/*"))
                    tokens.emplace_back(t, m.Start());
            }
            Ordered result;
            std::map<std::string, long long> positions;
            std::optional<std::string> reference;
            const auto lang = "LANG_" + Upper(language);
            for (size_t i = 0; i + 1 < tokens.size(); i++)
            {
                const auto& token = tokens[i].first;
                if (token == "REFERENCE")
                    reference = Unquote(tokens[i + 1].first);
                else if (token == lang && reference)
                {
                    const auto value = Unquote(tokens[i + 1].first);
                    auto it = std::find_if(result.begin(), result.end(), [&](const auto& kv) { return kv.first == *reference; });
                    if (it != result.end() && it->second != value && conflicts)
                        conflicts->push_back(Json{{"key", *reference},
                                                  {"previous_value", it->second},
                                                  {"value", value},
                                                  {"previous_line", lines[static_cast<size_t>(positions[*reference])] + 1},
                                                  {"line", lines[static_cast<size_t>(tokens[i].second)] + 1}});
                    if (it != result.end())
                        it->second = value;
                    else
                        result.emplace_back(*reference, value);
                    positions[*reference] = tokens[i].second;
                }
            }
            return result;
        }
    } // namespace

    std::map<std::string, std::string> ParseStringEd(const std::string& text, const std::string& language, Json* conflicts)
    {
        const auto ordered = Parse(text, language, conflicts);
        return {ordered.begin(), ordered.end()};
    }

    Json StageLocalization(const fs::path& requestPath)
    {
        const auto request = ReadJson(requestPath);
        const auto path = [&](const char* key) -> std::optional<fs::path>
        {
            if (!request.contains(key) || request[key].is_null())
                return std::nullopt;
            return files::FromUtf8(request[key].get<std::string>());
        };
        const auto paths = [&](const char* key)
        {
            std::vector<fs::path> out;
            for (const auto& p : request.value(key, Json::array()))
                out.push_back(files::FromUtf8(p.get<std::string>()));
            return out;
        };
        const auto project = *path("project");
        const auto roots = paths("roots"), iwdDirs = paths("iwd_dirs"), rawRoots = paths("raw_roots");
        const auto unlinker = path("unlinker"), bo2Root = path("bo2_root"), t6Unlinker = path("t6_unlinker"), t6StockDump = path("t6_stock_dump");
        const auto language = request.value("language", std::string("english"));
        const auto work = path("work").value_or(project / "content_source/localization_cache");
        auto stock = extract::StockWaw::FromJson(request.value("stock", Json()));

        std::map<std::string, std::string> reverse;
        const auto previous = project / "content_source/localization.stage.json";
        if (IsFile(previous))
            for (const auto& e : ReadJson(previous).value("entries", Json::array()))
                reverse[e["output"].get<std::string>()] = e["key"].get<std::string>();
        const auto original = [&](const std::string& text)
        {
            if (const auto it = reverse.find(text); it != reverse.end())
                return it->second;
            return StartsWith(text, PREFIX) ? text.substr(PREFIX.size()) : text;
        };
        std::map<std::string, std::pair<std::string, std::string>> sources;
        std::vector<std::string> sourceOrder; // dict insertion order
        Json conflicts = Json::array();
        std::set<std::tuple<std::string, std::string, int, int>> seenConflicts;
        std::set<std::string> requested, strings, hintKeys, formattedHintKeys;
        std::vector<std::pair<fs::path, std::vector<gsc::Token>>> scripts;
        std::vector<std::pair<fs::path, Json>> weaponFiles;
        for (const auto& [folder, ext] : {std::pair<const char*, const char*>{"maps/mp/waw", ".gsc"}, {"clientscripts/mp/waw", ".csc"}})
        {
            auto found = files::RGlob(project / folder, ext);
            SortPaths(found);
            for (const auto& p : found)
            {
                if (files::Utf8(p.filename()) == "_waw2bo2_compat.gsc") // this adapter uses BO2's own keys
                    continue;
                auto tokens = gsc::Tokenize(encoding::WawText(ReadBytes(p)));
                for (size_t index = 0; index < tokens.size(); index++)
                {
                    const auto& token = tokens[index];
                    if (token.kind != gsc::Kind::String)
                        continue;
                    auto stripped = token.text;
                    while (!stripped.empty() && (stripped[0] == '&' || stripped[0] == '#'))
                        stripped.erase(0, 1);
                    const auto text = Unquote(stripped);
                    if (StartsWith(token.text, "&\""))
                    {
                        requested.insert(original(text));
                        if (index >= 2 && tokens[index - 1].text == "(" && tokens[index - 2].Low() == "sethintstring")
                        {
                            // SetHintString( &"X", cost, ammo_cost ): the map fills &&1.. itself
                            const bool formatted = index + 1 < tokens.size() && tokens[index + 1].text == ",";
                            (formatted ? formattedHintKeys : hintKeys).insert(original(text));
                        }
                    }
                    else if (StartsWith(token.text, "\"") && IsKey(original(text)))
                        strings.insert(original(text)); // a plain string is a reference only in key form
                }
                scripts.emplace_back(p, std::move(tokens));
            }
        }
        {
            std::vector<fs::path> found;
            const auto folder = project / "content_source/weapons";
            std::error_code e;
            if (IsDir(folder))
                for (auto it = fs::recursive_directory_iterator(folder, e); it != fs::recursive_directory_iterator(); ++it)
                    found.push_back(it->path());
            SortPaths(found);
            for (const auto& p : found)
            {
                if (!IsFile(p))
                    continue;
                if (StartsWith(encoding::WawText(ReadBytes(p)), "FLAMETABLEFILE\\"))
                    continue; // nested simulation data, not a weapon display name
                auto fields = weapons::ReadInfo(p);
                for (const char* key : {"displayName", "AIOverlayDescription", "modeName"})
                    if (fields.contains(key) && !fields[key].get<std::string>().empty())
                        requested.insert(original(fields[key].get<std::string>()));
                weaponFiles.emplace_back(p, std::move(fields));
            }
        }
        const auto readStrings = [&](const std::string& text, const std::string& origin)
        {
            Json duplicates = Json::array();
            auto values = Parse(text, language, &duplicates);
            for (const auto& entry : duplicates)
            {
                const auto identity = std::tuple{origin, entry["key"].get<std::string>(), entry["previous_line"].get<int>(), entry["line"].get<int>()};
                if (seenConflicts.insert(identity).second)
                {
                    auto copy = entry;
                    copy["source"] = origin;
                    conflicts.push_back(copy);
                }
            }
            return values;
        };
        const auto add = [&](const std::string& text, const std::string& origin, const std::string& stem)
        {
            for (const auto& [key, value] : readStrings(text, origin))
            {
                if (sources.try_emplace(key, value, origin).second)
                    sourceOrder.push_back(key);
                // WaW raw StringEd files may use a file namespace and short refs
                if (sources.try_emplace(Upper(stem) + "_" + key, value, origin).second)
                    sourceOrder.push_back(Upper(stem) + "_" + key);
            }
        };
        const auto addRoot = [&](const fs::path& root)
        {
            for (const auto& p : SourceFiles(root, language))
                add(ReadSigReplace(p), Str(p), files::Utf8(p.stem()));
        };
        std::map<std::string, fs::path> prepared;
        for (const auto& root : roots)
        {
            const auto receipt = root / "extraction.json";
            if (IsFile(receipt))
                prepared[Str(extract::Resolve(files::FromUtf8(ReadJson(receipt)["inputs"]["file"].get<std::string>())))] = root;
        }
        if (unlinker)
        {
            std::vector<fs::path> zones;
            std::set<std::string> seen;
            for (const auto& directory : iwdDirs)
            {
                if (!IsDir(directory))
                    continue;
                std::error_code e;
                for (const auto& entry : fs::directory_iterator(directory, e))
                    if (Lower(files::Utf8(entry.path().extension())) == ".ff" && seen.insert(Lower(Str(entry.path()))).second)
                        zones.push_back(entry.path());
            }
            // patched definitions win; common localization is the map's baseline
            std::sort(zones.begin(), zones.end(), [](const fs::path& a, const fs::path& b)
                      {
                          const auto key = [](const fs::path& p)
                          {
                              const auto stem = Lower(files::Utf8(p.stem()));
                              return std::tuple(stem.find("patch") == std::string::npos, stem.find("common") != std::string::npos, files::Utf8(p.filename()));
                          };
                          return key(a) < key(b);
                      });
            std::vector<fs::path> pending;
            for (const auto& ff : zones)
                if (!prepared.count(Str(extract::Resolve(ff))))
                    pending.push_back(ff);
            std::vector<fs::path> dumps(pending.size());
            parallel::For(pending.size(), [&](const size_t i)
                          {
                              std::vector<fs::path> search{pending[i].parent_path()};
                              search.insert(search.end(), iwdDirs.begin(), iwdDirs.end());
                              dumps[i] = DumpZone(pending[i], *unlinker, work, search);
                          });
            size_t next = 0;
            for (const auto& ff : zones)
            {
                const auto it = prepared.find(Str(extract::Resolve(ff)));
                addRoot(it != prepared.end() ? it->second : dumps[next++]);
            }
        }
        for (const auto& root : roots)
            addRoot(root);
        for (const auto& directory : iwdDirs)
        {
            std::vector<fs::path> archives;
            if (IsFile(directory))
                archives.push_back(directory);
            else if (IsDir(directory))
            {
                std::error_code e;
                for (const auto& entry : fs::directory_iterator(directory, e))
                    if (Lower(files::Utf8(entry.path().extension())) == ".iwd")
                        archives.push_back(entry.path());
                SortPaths(archives);
            }
            for (const auto& archive : archives)
            {
                zip::Archive z(archive);
                std::vector<const zip::Entry*> entries;
                for (const auto& entry : z.Entries())
                    entries.push_back(&entry);
                std::sort(entries.begin(), entries.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
                for (const auto* entry : entries)
                {
                    auto low = Lower(entry->name);
                    std::replace(low.begin(), low.end(), '\\', '/');
                    if (low.size() >= 4 && low.substr(low.size() - 4) == ".str" && (StartsWith(low, "localizedstrings/") || StartsWith(low, language + "/localizedstrings/")))
                        add(encoding::Utf8SigReplace(z.Read(*entry)), Str(archive) + ":" + entry->name, files::Utf8(files::FromUtf8(entry->name).stem()));
                }
            }
        }
        strings.insert(ENGINE_STRINGS.begin(), ENGINE_STRINGS.end());
        // the translated WaW in-game menus' @ references (wawmenu.requested_strings)
        if (const auto manifest = project / "content_source/waw_menu.json"; IsFile(manifest))
        {
            // set(data['strings']): the keys of an object, the items of a list
            const auto data = ReadJson(manifest);
            const auto& listed = data["strings"];
            if (listed.is_object())
                for (const auto& [key, _] : listed.items())
                    strings.insert(key);
            else
                for (const auto& s : listed)
                    strings.insert(s.get<std::string>());
        }
        std::set<std::string> known;
        for (const auto& [key, _] : sources)
            known.insert(key);
        if (stock)
            for (const auto& [_, kinds] : stock->Index().items())
                if (kinds.contains("localize"))
                    for (const auto& n : kinds["localize"])
                        known.insert(n.get<std::string>());
        for (const auto& s : strings)
            if (known.count(s))
                requested.insert(s);
        if (stock)
        {
            std::vector<std::string> absent;
            for (const auto& key : requested)
                if (!sources.count(key))
                    absent.push_back(key);
            for (const auto& key : absent)
            {
                const auto zones = stock->ZonesDefining("localize", key);
                if (!zones.empty())
                    // the shared stock cache is dumped without localize assets
                    addRoot(DumpZone(stock->wawRoot / "zone" / "english" / files::FromUtf8(zones[0] + ".ff"), stock->unlinker, work, {stock->wawRoot / "main"}));
            }
        }
        for (const auto& root : rawRoots)
            addRoot(root);
        for (const auto& [key, _] : sources)
            known.insert(key);
        for (const auto& s : strings)
            if (known.count(s))
                requested.insert(s);
        // exact BO2 text is eligible only after the complete source search fails
        std::map<std::string, std::pair<std::string, std::string>> fallback;
        const auto addFallback = [&](const fs::path& root)
        {
            for (const auto& p : SourceFiles(root, language))
                for (const auto& [key, value] : readStrings(ReadSigReplace(p), Str(p)))
                    fallback.try_emplace(key, value, Str(p));
        };
        if (bo2Root)
        {
            if (t6StockDump)
                addFallback(*t6StockDump);
            else if (t6Unlinker)
                for (const std::string zone : {"patch_zm", "common_zm", "code_post_gfx_zm"})
                {
                    const auto ff = *bo2Root / "zone" / files::FromUtf8(language) / files::FromUtf8(language.substr(0, 2) + "_" + zone + ".ff");
                    if (IsFile(ff))
                        addFallback(DumpZone(ff, *t6Unlinker, work, {*bo2Root / "sound", ff.parent_path()}));
                }
            addFallback(*bo2Root / "raw");
        }
        Json entriesReport = Json::array(), missing = Json::array();
        std::map<std::string, std::string> entries, mapping;
        const auto compatibility = ReadJson(extract::ResourceRoot() / "src/waw2bo2/compat/bo2_equivalents.json");
        const auto equivalents = compatibility.value("localize", Json::object());
        const auto texts = compatibility.value("localize_text", Json::object());
        // WaW resolves localized references case-insensitively
        std::map<std::string, std::pair<std::string, std::string>> folded;
        for (const auto& candidate : sourceOrder)
            folded.try_emplace(Upper(candidate), sources[candidate]);
        for (const auto& key : requested)
        {
            std::optional<std::pair<std::string, std::string>> hit;
            std::string kind = "WaW";
            if (const auto it = sources.find(key); it != sources.end())
                hit = it->second;
            else if (const auto f = folded.find(Upper(key)); f != folded.end())
                hit = f->second;
            if (!hit)
                if ((hit = PriceVariant(key, sources)))
                    kind = "WAW_PRICE_VARIANT";
            if (!hit && fallback.count(key))
                hit = fallback[key], kind = "BO2_EXACT_FALLBACK";
            if (!hit && equivalents.contains(key) && equivalents[key].is_string() && fallback.count(equivalents[key].get<std::string>()))
                hit = fallback[equivalents[key].get<std::string>()], kind = "BO2_SHARED_FALLBACK";
            if (!hit && texts.contains(key))
                hit = std::pair{texts[key].get<std::string>(), std::string("compat/bo2_equivalents.json")}, kind = "COMPATIBILITY_TEXT_FALLBACK";
            if (!hit && !IsKey(key))
                hit = std::pair{key, std::string("literal display text")}, kind = "literal";
            if (!hit)
            {
                // WaW draws an unresolved reference as its key; so does the port
                missing.push_back(key);
                hit = std::pair{key, std::string("unresolved WaW reference")}, kind = "MISSING_SHOWN_AS_KEY";
            }
            auto [value, origin] = *hit;
            if (hintKeys.count(key) && !formattedHintKeys.count(key) && (kind == "WaW" || kind == "WAW_PRICE_VARIANT"))
                // WaW substitutes the activation key into &&1 for a hint set without arguments
                if (const auto at = value.find("&&1"); at != std::string::npos)
                    value.replace(at, 3, "^3[{+activate}]^7");
            const auto name = PREFIX + (Identifier(key) ? key : "TEXT_" + Upper(hash::Sha256Hex(key).substr(0, 20)));
            entries[name] = value;
            mapping[key] = name;
            entriesReport.push_back(Json{{"key", key}, {"output", name}, {"value", value}, {"source", origin}, {"kind", kind}});
        }
        Json rewritten = Json::array();
        for (auto& [p, tokens] : scripts)
        {
            const auto before = gsc::Emit(tokens);
            for (auto& token : tokens)
            {
                if (token.kind != gsc::Kind::String || StartsWith(token.text, "#"))
                    continue;
                auto stripped = token.text;
                while (!stripped.empty() && stripped[0] == '&')
                    stripped.erase(0, 1);
                const auto text = original(Unquote(stripped));
                const bool localized = StartsWith(token.text, "&");
                if (mapping.count(text) && (localized || (known.count(text) && strings.count(text))))
                    token.text = (localized ? "&" : "") + Quote(mapping[text]);
            }
            const auto after = gsc::Emit(tokens);
            if (after != before)
            {
                files::WriteText(p, after);
                auto relative = files::Utf8(p.lexically_relative(project));
                std::replace(relative.begin(), relative.end(), '\\', '/');
                rewritten.push_back(relative);
            }
        }
        for (auto& [p, fields] : weaponFiles)
        {
            for (const char* field : {"displayName", "AIOverlayDescription", "modeName"})
            {
                const auto key = original(fields.contains(field) ? fields[field].get<std::string>() : std::string());
                if (mapping.count(key))
                    fields[field] = mapping[key];
            }
            files::WriteText(p, weapons::WriteInfo(fields));
        }
        const auto asset = "waw_" + files::Utf8(project.filename());
        const auto destination = project / files::FromUtf8(language) / "localizedstrings" / files::FromUtf8(asset + ".str");
        fs::create_directories(destination.parent_path());
        std::string text = "VERSION \"1\"\nCONFIG \"\"\nFILENOTES \"WaW source UI strings\"\n";
        for (const auto& [name, value] : entries)
            text += "\nREFERENCE " + name + "\nLANG_" + Upper(language) + " " + Quote(value) + "\n";
        files::WriteText(destination, text + "\nENDMARKER\n");
        Json warnings = Json::array();
        for (const auto& key : missing)
            warnings.push_back("LOCALIZATION_MISSING " + key.get<std::string>() + ": no source text in the map, its IWDs or stock WaW; shown as the key, as WaW does");
        for (const auto& e : conflicts)
            warnings.push_back("LOCALIZATION_CONFLICT " + e["key"].get<std::string>() + ": " + e["source"].get<std::string>() + " lines " +
                               std::to_string(e["previous_line"].get<int>()) + "/" + std::to_string(e["line"].get<int>()) + "; later definition retained within that file");
        const Json report{{"asset", asset}, {"language", language}, {"entries", entriesReport}, {"rewritten_scripts", rewritten}, {"missing", missing},
                          {"errors", Json::array()}, {"conflicts", conflicts}, {"warnings", warnings}};
        const auto output = project / "content_source/localization.stage.json";
        fs::create_directories(output.parent_path());
        files::WriteText(output, py::JsonDumps(report, 2) + "\n");
        return report;
    }
} // namespace w2b::strings
