// The sounds the map plays (see sound_refs.h).
#include "sound_refs.h"

#include "amb_stage.h"
#include "assets/resolver.h"
#include "core/encoding.h"
#include "core/pyre.h"
#include "csvtable.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "gsc/tokens.h"
#include "modutil.h"
#include "sound/bank.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::IsDir;
    using mod::IsFile;
    using mod::Lower;
    using mod::StartsWith;
    using mod::Str;
    using py::Json;

    namespace
    {
        using Names = std::set<std::string>;

        bool IsAlnum(const char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (static_cast<unsigned char>(c) & 0x80);
        }

        bool IsLowerAlnum(const char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        }

        // _alias_words: the [a-z0-9] runs of a lower-cased name
        std::vector<std::string> AliasWords(const std::string& lowName)
        {
            std::vector<std::string> out;
            std::string word;
            for (const char c : lowName)
                if (IsLowerAlnum(c))
                    word += c;
                else if (!word.empty())
                {
                    out.push_back(word);
                    word.clear();
                }
            if (!word.empty())
                out.push_back(word);
            return out;
        }

        bool IsDigits(const std::string& w)
        {
            return !w.empty() && std::all_of(w.begin(), w.end(), [](const char c) { return c >= '0' && c <= '9'; });
        }

        std::string Strip(const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && py::IsSpace(s[a]))
                a++;
            while (b > a && py::IsSpace(s[b - 1]))
                b--;
            return s.substr(a, b - a);
        }

        struct Literal
        {
            std::string text;
            bool preceded, followed;
        };

        // _string_literals: (text, preceded by +, followed by +)
        std::vector<Literal> StringLiterals(const std::string& source)
        {
            std::vector<Literal> out;
            std::vector<gsc::Token> tokens;
            try
            {
                tokens = gsc::Tokenize(source);
            }
            catch (const gsc::GscSyntaxError&)
            {
                static const pyre::Regex string("(\\+\\s*)?\"((?:\\\\.|[^\"\\\\\\n])*)\"(\\s*\\+)?");
                for (const auto& m : string.FindIter(source))
                    out.push_back({*m.Group(2), m.Group(1).has_value(), m.Group(3).has_value()});
                return out;
            }
            for (size_t i = 0; i < tokens.size(); i++)
                if (tokens[i].kind == gsc::Kind::String)
                {
                    auto text = tokens[i].text;
                    const auto start = text.find_first_not_of("&#");
                    text = start == std::string::npos ? std::string() : text.substr(start);
                    text = text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string();
                    out.push_back({text, i > 0 && tokens[i - 1].text == "+", i + 1 < tokens.size() && tokens[i + 1].text == "+"});
                }
            return out;
        }

        // {name.lower(): name for name in sorted(defined)}
        std::map<std::string, std::string> ByLower(const Names& defined)
        {
            std::map<std::string, std::string> out;
            for (const auto& name : defined)
                out[Lower(name)] = name;
            return out;
        }

        bool Under(const fs::path& path, const fs::path& root)
        {
            std::error_code e;
            const auto a = Lower(Str(fs::weakly_canonical(path, e))), b = Lower(Str(fs::weakly_canonical(root, e)));
            if (a == b)
                return true;
            return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (b.back() == '\\' || a[b.size()] == '\\');
        }

        // sounds.script_aliases
        Names ScriptAliases(const fs::path& projectRoot, const Names& defined, const Names& dynamic)
        {
            const auto names = ByLower(defined);
            Names found, literals, heads, fragments;
            for (const char* suffix : {".gsc", ".csc"})
                for (const auto& path : files::GlobSuffix(projectRoot, suffix))
                    for (const auto& literal : StringLiterals(encoding::WawText(mod::Bytes(path))))
                    {
                        auto text = Lower(literal.text);
                        if (StartsWith(text, "waw/"))
                            text = text.substr(4);
                        literals.insert(text);
                        if (const auto it = names.find(text); it != names.end())
                            found.insert(it->second);
                        const auto alnum = std::count_if(text.begin(), text.end(), IsLowerAlnum);
                        if (!(literal.preceded || literal.followed) || alnum < 2)
                            continue;
                        if (literal.followed && !literal.preceded && !text.empty() && IsAlnum(text[0]))
                            heads.insert(text);
                        else
                            fragments.insert(text);
                    }
            std::vector<std::string> tables;
            for (const auto& t : literals)
                if (mod::EndsWith(t, ".csv"))
                    tables.push_back(t);
            for (const auto& text : tables)
            {
                auto relative = text;
                std::replace(relative.begin(), relative.end(), '\\', '/');
                const auto table = projectRoot / files::FromUtf8(relative);
                if (!IsFile(table) || !Under(table, projectRoot))
                    continue;
                for (const auto& row : ReadCsv(encoding::Utf8Replace(mod::Bytes(table))))
                    for (const auto& cell : row)
                    {
                        const auto low = Lower(Strip(cell));
                        if (const auto it = names.find(low); it != names.end())
                            found.insert(it->second);
                        literals.insert(low);
                    }
            }
            if (!dynamic.empty() && (!heads.empty() || !fragments.empty()))
            {
                Names words;
                for (const auto& text : literals)
                    for (const auto& w : AliasWords(text))
                        words.insert(w);
                for (const auto& name : dynamic)
                {
                    const auto low = Lower(name);
                    if (found.count(name))
                        continue;
                    const auto parts = AliasWords(low);
                    if (!std::all_of(parts.begin(), parts.end(), [&](const std::string& w) { return IsDigits(w) || words.count(w); }))
                        continue;
                    const bool head = std::any_of(heads.begin(), heads.end(), [&](const std::string& h) { return StartsWith(low, h); });
                    const bool fragment = std::any_of(fragments.begin(), fragments.end(), [&](const std::string& f) { return low.find(f) != std::string::npos; });
                    if (head || fragment)
                        found.insert(name);
                }
            }
            return found;
        }

        // entities.parse_entities: dict(KV_RE.findall(body)) per entity
        std::vector<std::vector<std::pair<std::string, std::string>>> ParseEntities(const std::string& text)
        {
            static const pyre::Regex entity("\\{([^{}]*)\\}"), kv("\"([^\"]*)\"\\s+\"([^\"]*)\"");
            std::vector<std::vector<std::pair<std::string, std::string>>> out;
            for (const auto& body : entity.FindAll(text))
            {
                std::vector<std::pair<std::string, std::string>> fields;
                for (const auto& pair : kv.FindAll(body[0]))
                {
                    const auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.first == pair[0]; });
                    if (it == fields.end())
                        fields.emplace_back(pair[0], pair[1]);
                    else
                        it->second = pair[1];
                }
                out.push_back(std::move(fields));
            }
            return out;
        }

        // sounds.entity_aliases
        Names EntityAliases(const std::optional<fs::path>& ents, const Names& defined)
        {
            Names out;
            if (!ents || !IsFile(*ents))
                return out;
            const auto names = ByLower(defined);
            for (const auto& entity : ParseEntities(encoding::WawText(mod::Bytes(*ents))))
                for (const auto& [_, value] : entity)
                    if (const auto it = names.find(Lower(value)); it != names.end())
                        out.insert(it->second);
            return out;
        }

        // sounds.CLIENT_AUDIO_ENTITIES
        const std::vector<std::tuple<std::string, std::vector<std::string>, std::vector<std::string>>> CLIENT_AUDIO_ENTITIES{
            {"script_label", {"line_emitter", "looper", "random"}, {"script_sound"}},
            {"targetname", {"audio_bump_trigger"}, {"script_sound", "script_noteworthy"}},
            {"targetname", {"audio_step_trigger"}, {"script_label", "script_sound", "script_noteworthy"}}};

        // sounds.bind_entity_sounds
        long long BindEntitySounds(const fs::path& entitiesJson, const Names& converted)
        {
            if (!IsFile(entitiesJson))
                return 0;
            auto data = mod::Json(entitiesJson);
            const auto names = ByLower(converted);
            long long count = 0;
            for (auto& entity : data["entities"])
            {
                std::vector<std::string> keys;
                for (const auto& [field, values, ks] : CLIENT_AUDIO_ENTITIES)
                    if (entity.contains(field) && entity[field].is_string() && std::find(values.begin(), values.end(), entity[field].get<std::string>()) != values.end())
                        keys.insert(keys.end(), ks.begin(), ks.end());
                for (const auto& key : keys)
                {
                    if (!entity.contains(key) || !entity[key].is_string())
                        continue;
                    const auto it = names.find(Lower(entity[key].get<std::string>()));
                    if (it != names.end())
                    {
                        entity[key] = "waw/" + it->second;
                        count++;
                    }
                }
            }
            if (count)
                files::WriteText(entitiesJson, py::JsonDumps(data, 1) + "\n");
            return count;
        }

        // sounds.bind_script_sound_comparisons
        long long BindScriptSoundComparisons(const fs::path& entitiesJson, const fs::path& scriptsRoot)
        {
            if (!IsFile(entitiesJson) || !IsDir(scriptsRoot))
                return 0;
            std::map<std::string, std::string> bound;
            Names raw;
            const auto data = mod::Json(entitiesJson);
            const auto entities = data["entities"];
            for (const auto& entity : entities)
            {
                if (!entity.contains("script_sound") || !entity["script_sound"].is_string())
                    continue;
                const auto value = entity["script_sound"].get<std::string>();
                if (StartsWith(value, "waw/"))
                    bound[Lower(value.substr(4))] = value;
                else
                    raw.insert(Lower(value));
            }
            std::map<std::string, std::string> names;
            for (const auto& [k, v] : bound)
                if (!raw.count(k))
                    names[k] = v;
            if (names.empty())
                return 0;
            static const pyre::Regex compare("(?P<lead>\\bscript_sound\\s*[!=]=\\s*)\"(?P<a>[^\"\\\\\\n]+)\"|\"(?P<b>[^\"\\\\\\n]+)\"(?P<tail>\\s*[!=]=\\s*[\\w.\\[\\]]*\\bscript_sound\\b)");
            long long count = 0;
            const auto swap = [&](const pyre::Match& m) -> std::string
            {
                const auto a = m.Group(std::string("a"));
                const auto literal = a ? *a : *m.Group(std::string("b"));
                const auto it = names.find(Lower(literal));
                if (it == names.end())
                    return *m.Group(0);
                count++;
                return a ? *m.Group(std::string("lead")) + "\"" + it->second + "\"" : "\"" + it->second + "\"" + *m.Group(std::string("tail"));
            };
            for (const auto& path : files::GlobSuffix(scriptsRoot, ".gsc"))
            {
                const auto text = files::ReadText(path);
                const auto updated = compare.Sub(swap, text);
                if (updated != text)
                    files::WriteText(path, updated);
            }
            return count;
        }

        // animnotes.bind_sound_events: sndnt# notes take the converted waw/ alias
        std::optional<std::pair<std::string, Json>> BindSoundEvents(const std::string& raw, const Names& aliases)
        {
            if (raw.find("sndnt#") == std::string::npos)
                return std::nullopt;
            uint16_t version = 0;
            if (raw.size() >= 2)
                std::memcpy(&version, raw.data(), 2);
            if (raw.size() < 4 || version != 17)
                throw StepError("WeaponError", "sound notifications require a raw v17 animation");
            const auto start = sound::NotificationOffsetOf(raw);
            if (start >= raw.size())
                throw StepError("IndexError", "index out of range");
            size_t pos = start + 1;
            std::string notes;
            Json changed = Json::array();
            for (int n = 0; n < static_cast<unsigned char>(raw[start]); n++)
            {
                const auto end = raw.find('\0', pos);
                if (end == std::string::npos)
                    throw StepError("ValueError", "subsection not found");
                auto name = raw.substr(pos, end - pos);
                const auto frame = raw.substr(end + 1, 2);
                const auto alias = StartsWith(name, "sndnt#") ? name.substr(6) : name;
                if (StartsWith(name, "sndnt#") && aliases.count(alias) && !StartsWith(alias, "waw/"))
                {
                    uint16_t f = 0;
                    std::memcpy(&f, frame.data(), std::min<size_t>(2, frame.size()));
                    changed.push_back(Json{{"source", alias}, {"sound", "waw/" + alias}, {"frame", f}});
                    name = "sndnt#waw/" + alias;
                }
                notes += name + std::string(1, '\0') + frame;
                pos = end + 3;
            }
            if (pos != raw.size())
                throw StepError("WeaponError", "raw animation has no valid notification suffix");
            if (changed.empty())
                return std::nullopt;
            return std::pair{raw.substr(0, start + 1) + notes, changed};
        }

        // animnotes.bind_actor_sounds
        Json BindActorSounds(const fs::path& project, const Names& aliases)
        {
            Json report = Json::array();
            const auto root = project / "xanim";
            for (const auto& path : files::GlobSuffix(root, ""))
            {
                const auto relative = mod::Generic(path.lexically_relative(root));
                if (!StartsWith(relative, "waw_anim_"))
                    continue;
                if (const auto bound = BindSoundEvents(mod::Bytes(path), aliases))
                {
                    files::WriteBytes(path, bound->first);
                    report.push_back(Json{{"animation", relative}, {"events", bound->second}});
                }
            }
            return report;
        }

        Names StringSet(const Json& v)
        {
            Names out;
            for (const auto& s : v)
                out.insert(s.get<std::string>());
            return out;
        }
    } // namespace

    Json DiscoverSounds(const Json& request)
    {
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        auto stockWaw = extract::StockWaw::FromJson(request.value("stock_waw", Json()));
        const auto zoneSounds = StringSet(request["zone_sounds"]);
        auto compiled = StringSet(request["compiled_sounds"]);
        const std::optional<fs::path> ents = request["ents"].is_null() ? std::nullopt : std::optional(files::FromUtf8(request["ents"].get<std::string>()));
        Names stockSounds;
        if (stockWaw)
            for (const auto& [_, kinds] : stockWaw->Index().items())
                if (kinds.contains("sound"))
                    for (const auto& n : kinds["sound"])
                        stockSounds.insert(n.get<std::string>());
        Names defined = zoneSounds;
        defined.insert(compiled.begin(), compiled.end());
        defined.insert(stockSounds.begin(), stockSounds.end());
        auto scriptSounds = ScriptAliases(projectRoot, defined, zoneSounds);
        if (!request["source_amb"].is_null())
            for (const auto& a : AmbientAliases(encoding::WawText(mod::Bytes(files::FromUtf8(request["source_amb"].get<std::string>())))))
                if (defined.count(a))
                    scriptSounds.insert(a);
        const auto entitySounds = EntityAliases(ents, defined);
        compiled.insert(scriptSounds.begin(), scriptSounds.end());
        compiled.insert(entitySounds.begin(), entitySounds.end());
        compiled.insert(zoneSounds.begin(), zoneSounds.end());
        Json content = Json::object();
        content["script_sound_dependencies"] = std::vector<std::string>(scriptSounds.begin(), scriptSounds.end());
        content["entity_sound_dependencies"] = std::vector<std::string>(entitySounds.begin(), entitySounds.end());
        // assetresolve.Resolver(roots, stock_waw).expand, the stock dumps in process
        Json requests = Json::array();
        for (const auto& n : compiled)
            requests.push_back(Json::array({"sound", n}));
        const Json expandRequest{{"base", Str(extract::ResourceRoot())}, {"sourceRoots", request["roots"]}, {"stockRoots", Json::array()},
                                 {"nodes", Json::array()}, {"edges", Json::array()}, {"requests", requests}, {"hasStock", stockWaw.has_value()}};
        assets::StockLookup lookup;
        assets::StockSounds sounds;
        if (stockWaw)
        {
            lookup = [&](const assets::Asset& asset) -> std::optional<assets::StockRoot>
            {
                const auto hit = stockWaw->RootFor(asset.first, asset.second);
                if (!hit)
                    return std::nullopt;
                return assets::StockRoot{hit->first, hit->second};
            };
            sounds = [&]() { return stockSounds; };
        }
        auto result = assets::Expand(expandRequest, std::move(lookup), std::move(sounds));
        auto graph = result["report"];
        // the report's missing rows are its node rows (Resolver.expand)
        std::set<std::pair<std::string, std::string>> missing;
        for (const auto& node : graph["missing"])
            missing.insert({node["kind"].get<std::string>(), node["name"].get<std::string>()});
        Json missingRows = Json::array();
        Names soundNames;
        for (const auto& node : graph["nodes"])
        {
            if (missing.count({node["kind"].get<std::string>(), node["name"].get<std::string>()}))
                missingRows.push_back(node);
            if (node["kind"] == "sound")
                soundNames.insert(node["name"].get<std::string>());
        }
        graph["missing"] = missingRows;
        content["entity_sounds_bound"] = BindEntitySounds(projectRoot / "BSP" / "entities.json", soundNames);
        content["script_sound_comparisons_bound"] = BindScriptSoundComparisons(projectRoot / "BSP" / "entities.json", projectRoot / "maps");
        std::vector<std::string> unreferenced;
        for (const auto& n : zoneSounds)
            if (!soundNames.count(n))
                unreferenced.push_back(n);
        fs::create_directories(projectRoot / "content_source");
        files::WriteText(projectRoot / "content_source/sounds.unreferenced.json", py::JsonDumps(Json(unreferenced), 2) + "\n");
        content["unreferenced_sound_aliases"] = Json{{"count", unreferenced.size()}, {"defined", zoneSounds.size()}, {"report", "content_source/sounds.unreferenced.json"}};
        return Json{{"script_sounds", std::vector<std::string>(scriptSounds.begin(), scriptSounds.end())},
                    {"entity_sounds", std::vector<std::string>(entitySounds.begin(), entitySounds.end())},
                    {"compiled_sounds", std::vector<std::string>(compiled.begin(), compiled.end())},
                    {"sound_names", std::vector<std::string>(soundNames.begin(), soundNames.end())},
                    {"sound_graph", graph},
                    {"stock_roots", result["stockRoots"]},
                    {"animation_sound_identities", BindActorSounds(projectRoot, soundNames)},
                    {"content", content},
                    {"stock_prepared", stockWaw ? stockWaw->PreparedJson() : Json()}};
    }

    Json DiscoverSoundsRequest(const fs::path& request)
    {
        return DiscoverSounds(mod::Json(request));
    }
} // namespace w2b::pipeline
