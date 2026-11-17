// Port of gscport.Sources and gscport.is_core.
#include "sources.h"

#include "api.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/pyre.h"
#include "core/zip.h"

#include <algorithm>
#include <set>

namespace w2b::gsc::port
{
    namespace fs = std::filesystem;

    namespace
    {
        // Path.read_text(encoding, errors="replace"): universal newlines
        std::optional<std::string> ReadText(const fs::path& path, const bool sig)
        {
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                return std::nullopt;
            return encoding::UniversalNewlines(sig ? encoding::Utf8SigReplace(bytes) : encoding::Utf8Replace(bytes));
        }

        std::string SuffixLower(const std::string& name)
        {
            const auto slash = name.find_last_of("/\\");
            const auto base = slash == std::string::npos ? name : name.substr(slash + 1);
            const auto dot = base.rfind('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 == base.size())
                return "";
            return Lower(base.substr(dot));
        }

        // sorted(list(root.rglob("*.gsc")) + list(root.rglob("*.csc")))
        std::vector<fs::path> Scripts(const fs::path& root)
        {
            auto found = files::GlobSuffix(root, ".gsc");
            const auto csc = files::GlobSuffix(root, ".csc");
            found.insert(found.end(), csc.begin(), csc.end());
            std::vector<std::pair<std::vector<std::wstring>, fs::path>> keyed;
            for (auto& path : found)
            {
                std::vector<std::wstring> parts;
                for (const auto& part : path)
                {
                    auto w = part.wstring();
                    for (auto& c : w)
                        if (c >= L'A' && c <= L'Z')
                            c = static_cast<wchar_t>(c - L'A' + L'a');
                    parts.push_back(std::move(w));
                }
                keyed.emplace_back(std::move(parts), path);
            }
            std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<fs::path> out;
            for (auto& [_, path] : keyed)
            {
                std::error_code error;
                if (fs::is_regular_file(path, error))
                    out.push_back(path);
            }
            return out;
        }

        std::string Posix(const fs::path& relative)
        {
            const auto generic = relative.generic_u8string();
            return std::string(reinterpret_cast<const char*>(generic.data()), generic.size());
        }

        bool SamePath(const fs::path& a, const fs::path& b)
        {
            return Lower(files::Utf8(a.lexically_normal())) == Lower(files::Utf8(b.lexically_normal()));
        }
    } // namespace

    Sources::Sources(const std::vector<fs::path>& roots, const std::vector<fs::path>& iwds, const std::optional<fs::path>& stockRoot,
                     const std::vector<fs::path>& variants)
        : text(std::make_shared<TextDict>()),
          origin(std::make_shared<TextDict>()),
          stock(std::make_shared<TextDict>()),
          parsed(std::make_shared<std::unordered_map<std::string, ScriptPtr>>()),
          variantRoots(variants)
    {
        std::error_code error;
        if (stockRoot && fs::is_regular_file(*stockRoot / "catalog.json", error))
        {
            std::string data;
            files::ReadBytes(*stockRoot / "catalog.json", data);
            const auto catalog = py::Json::parse(data);
            if (catalog.contains("zones"))
                for (const auto& [_, entry] : catalog["zones"].items())
                {
                    const auto folder = files::FromUtf8(entry["folder"].get<std::string>());
                    if (fs::is_directory(folder, error)
                        && std::none_of(variantRoots.begin(), variantRoots.end(), [&](const fs::path& r) { return SamePath(r, folder); }))
                        variantRoots.push_back(folder);
                }
        }
        for (const auto& iwd : iwds)
        {
            zip::Archive archive(iwd);
            std::vector<const zip::Entry*> entries;
            for (const auto& entry : archive.Entries())
                entries.push_back(&entry);
            std::stable_sort(entries.begin(), entries.end(), [](const zip::Entry* a, const zip::Entry* b) { return a->name < b->name; });
            for (const auto* entry : entries)
            {
                const auto low = Lower(entry->name);
                if (!EndsWith(low, ".gsc") && !EndsWith(low, ".csc"))
                    continue;
                const auto key = Norm(entry->name) + SuffixLower(entry->name);
                if (text->Has(key))
                    continue;
                text->Set(key, encoding::UniversalNewlines(encoding::Utf8SigReplace(archive.Read(*entry))));
                origin->Set(key, files::Utf8(iwd.filename()) + ":" + entry->name);
            }
        }
        for (const auto& root : roots)
        {
            if (!fs::exists(root, error))
                continue;
            for (const auto& f : Scripts(root))
            {
                const auto key = Norm(Posix(f.lexically_relative(root))) + SuffixLower(files::Utf8(f.filename()));
                if (text->Has(key))
                    continue;
                text->Set(key, ReadText(f, true).value_or(""));
                origin->Set(key, files::Utf8(f.lexically_normal().make_preferred()));
            }
        }
        if (stockRoot && fs::exists(*stockRoot, error))
            for (const auto& f : Scripts(*stockRoot))
                stock->Set(Norm(Posix(f.lexically_relative(*stockRoot))) + SuffixLower(files::Utf8(f.filename())), ReadText(f, false).value_or(""));
    }

    const std::string* Sources::Source(const std::string& key) const
    {
        if (const auto* own = text->Find(key))
            return own;
        return stock->Find(key);
    }

    ScriptPtr Sources::Get(const std::string& path, const std::string& ext)
    {
        const auto key = Norm(path) + ext;
        const auto found = parsed->find(key);
        if (found != parsed->end())
            return found->second;
        std::optional<std::string> src;
        if (const auto* s = Source(key))
            src = *s;
        if (!src)
        {
            for (const auto& root : variantRoots)
            {
                const auto raw = root / files::FromUtf8(Replace(Norm(path), "\\", "/") + ext);
                std::error_code error;
                if (fs::is_regular_file(raw, error))
                {
                    src = ReadText(raw, true);
                    stock->Set(key, *src);
                    break;
                }
            }
        }
        ScriptPtr script = src ? std::make_shared<Script>(Parse(*src, Norm(path))) : nullptr;
        (*parsed)[key] = script;
        return script;
    }

    std::vector<std::string> Sources::StockVariants(const std::string& path, const std::string& ext)
    {
        const auto key = Norm(path) + ext;
        std::vector<std::string> texts;
        if (const auto* s = stock->Find(key))
            texts.push_back(*s);
        for (const auto& root : variantRoots)
        {
            const auto f = root / files::FromUtf8(Replace(Norm(path), "\\", "/") + ext);
            std::error_code error;
            if (fs::is_regular_file(f, error))
                texts.push_back(ReadText(f, false).value_or(""));
        }
        return texts;
    }

    bool Sources::IsStock(const std::string& path, const std::string& ext)
    {
        const auto key = Norm(path) + ext;
        if (!stock->Has(key))
            for (const auto& root : variantRoots)
            {
                const auto raw = root / files::FromUtf8(Replace(Norm(path), "\\", "/") + ext);
                std::error_code error;
                if (fs::is_regular_file(raw, error))
                {
                    stock->Set(key, ReadText(raw, false).value_or(""));
                    break;
                }
            }
        return stock->Has(key);
    }

    std::map<std::string, std::string> Sources::BindZombieFramework(const std::string& mainPath)
    {
        const auto script = Get(mainPath);
        const Function* main = script ? script->Find("main") : nullptr;
        if (!main)
            return {};
        std::set<std::string> candidates;
        for (const auto& ref : References(script->tokens, main->bodyOpen, main->bodyClose))
        {
            if (!ref.qualifier || ref.pointer || Lower(ref.name) != "main")
                continue;
            const auto target = Norm(*ref.qualifier);
            if (target == ZOMBIEMODE)
            {
                candidates.insert(target);
                continue;
            }
            const auto dependency = Get(target);
            static const std::vector<std::string> markers{"init_levelvars", "round_start", "round_think", "onplayerconnect", "player_damage_override"};
            if (!dependency || !std::all_of(markers.begin(), markers.end(), [&](const std::string& m) { return dependency->Find(m) != nullptr; }))
                continue;
            const Function* entry = dependency->Find("main");
            if (!entry || !entry->params.empty())
                continue;
            const auto refs = References(dependency->tokens, entry->bodyOpen, entry->bodyClose);
            const bool loadsMap = std::any_of(refs.begin(), refs.end(), [](const Ref& r)
                                              { return r.qualifier && Norm(*r.qualifier) == "maps\\_load" && Lower(r.name) == "main" && !r.pointer; });
            const bool waitsPlayers = std::any_of(refs.begin(), refs.end(),
                                                  [&](const Ref& r)
                                                  {
                                                      return Lower(r.name) == "flag_wait" && !r.pointer && dependency->tokens.at(r.index + 2).kind == Kind::String
                                                             && Lower(StripChars(dependency->tokens.at(r.index + 2).text, "\"")) == "all_players_connected";
                                                  });
            if (loadsMap && waitsPlayers)
                candidates.insert(target);
        }
        if (candidates.size() > 1)
        {
            std::string list;
            for (const auto& c : candidates)
                list += (list.empty() ? "'" : ", '") + Replace(c, "\\", "\\\\") + "'";
            throw std::runtime_error(mainPath + ": multiple zombie startup frameworks: [" + list + "]");
        }
        if (candidates.empty() || (candidates.size() == 1 && *candidates.begin() == ZOMBIEMODE))
            return {};
        const auto alias = *candidates.begin();
        const auto key = alias + ".gsc";
        const std::string* source = Source(key);
        const std::string sourceText = source ? *source : std::string();
        text = std::make_shared<TextDict>(*text);
        origin = std::make_shared<TextDict>(*origin);
        text->Set(std::string(ZOMBIEMODE) + ".gsc", sourceText);
        const auto* aliasOrigin = origin->Find(key);
        origin->Set(std::string(ZOMBIEMODE) + ".gsc", aliasOrigin ? *aliasOrigin : alias);
        for (const auto& path : std::vector<std::string>(text->order))
        {
            auto tokens = Tokenize(*text->Find(path));
            bool changed = false;
            for (const auto& ref : References(tokens))
                if (ref.qualifier && Norm(*ref.qualifier) == alias)
                {
                    tokens[*ref.qualIndex].text = ZOMBIEMODE;
                    changed = true;
                }
            static const pyre::Regex include(R"(#include\s+([\w\\/]+)\s*;?)", pyre::IGNORECASE);
            for (auto& token : tokens)
            {
                if (token.kind != Kind::Directive)
                    continue;
                const auto m = include.MatchAt(token.text);
                if (m && Norm((*m)[1]) == alias)
                {
                    token.text = std::string("#include ") + ZOMBIEMODE + ";";
                    changed = true;
                }
            }
            if (changed)
                text->Set(path, Emit(tokens));
        }
        parsed = std::make_shared<std::unordered_map<std::string, ScriptPtr>>();
        return {{alias, ZOMBIEMODE}};
    }

    bool IsCore(const std::string& path, Sources& sources)
    {
        const auto p = Norm(path);
        const auto& api = Api();
        if (api.contains("preserved_scripts"))
            for (const auto& s : api["preserved_scripts"])
                if (s == p)
                    return false;
        if (api["core_scripts"].contains(p))
            return true;
        for (const auto& s : api["template_scripts"])
            if (s == p)
                return true;
        static const std::vector<pyre::Regex> patterns = [&]
        {
            std::vector<pyre::Regex> out;
            for (const auto& pattern : api["core_patterns"])
                out.emplace_back(pattern.get<std::string>());
            return out;
        }();
        // any(re.search(r, p) for r in core_patterns) and sources.is_stock(p)
        for (const auto& re : patterns)
            if (re.Search(p))
                return sources.IsStock(p);
        return false;
    }
} // namespace w2b::gsc::port
