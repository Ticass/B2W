#include "pipeline/bridge_inventory.h"

#include "core/files.h"
#include "core/regex.h"
#include "mapents/entities.h"
#include "paths/paths.h"
#include "pipeline/modutil.h"
#include "weapons/plan.h"

#include <algorithm>
#include <set>

namespace w2b::pipeline
{
    namespace
    {
        using util::Json;
        using Names = std::set<std::string>;

        Names SoundAliases(const std::vector<fs::path>& roots)
        {
            constexpr std::string_view suffix = ".w2bsnd.json";
            Names names;
            for (const auto& root : roots)
            {
                const auto folder = root / "soundaliases";
                for (const auto& path : files::RGlob(folder, suffix))
                {
                    auto name = mod::Generic(path.lexically_relative(folder));
                    if (name.ends_with(suffix))
                        name.resize(name.size() - suffix.size());
                    names.insert(std::move(name));
                }
            }
            return names;
        }

        Names ShippedModels(const std::vector<fs::path>& roots)
        {
            Names models;
            for (const auto& root : roots)
            {
                const auto folder = root / "xmodel";
                for (const auto& path : files::RGlob(folder, ".json"))
                    models.insert(mod::Generic(path.lexically_relative(folder).replace_extension()));
            }
            return models;
        }

        void ReadIncludedWeapons(const std::string& source, Names& names)
        {
            constexpr std::string_view token = "include_weapon";
            constexpr std::string_view whitespace = " \t\n\r\f\v\x1c\x1d\x1e\x1f";
            const auto lowercase = mod::Lower(source);
            if (lowercase.find(token.substr(1)) == std::string::npos)
                return;
            // Unicode text needs Python's case folding and word-boundary rules.
            if (std::ranges::any_of(source, [](unsigned char c) { return c >= 0x80; }))
            {
                static const regex::Regex include("\\b[iI\xC4\xB0\xC4\xB1]" R"re(nclude_weapon\s*\(\s*"([^"]+)")re", regex::IGNORECASE);
                for (const auto& match : include.FindAll(source))
                    names.insert(match[0]);
                return;
            }

            for (std::size_t start = 0;;)
            {
                const auto position = lowercase.find(token, start);
                if (position == std::string::npos)
                    break;
                start = position + token.size();
                if (position && util::IsWord(source[position - 1]))
                    continue;
                const auto paren = source.find_first_not_of(whitespace, start);
                if (paren == std::string::npos || source[paren] != '(')
                    continue;
                const auto quote = source.find_first_not_of(whitespace, paren + 1);
                if (quote == std::string::npos || source[quote] != '"')
                    continue;
                const auto end = source.find('"', quote + 1);
                if (end == std::string::npos || end == quote + 1)
                    continue;
                names.insert(source.substr(quote + 1, end - quote - 1));
                start = end + 1;
            }
        }

        Names NamedWeapons(const fs::path& entities, const std::vector<fs::path>& mapRoots)
        {
            Names names;
            for (const auto& entity : mapents::ReadMapEntities(entities))
            {
                const auto cls = mapents::Field(entity, "classname");
                if (cls.starts_with("weapon_") && cls.size() > 7)
                    names.emplace(cls.substr(7));
                else if (cls == "misc_turret")
                    if (const auto name = mapents::Field(entity, "weaponinfo"); !name.empty())
                        names.emplace(name);
            }
            for (const auto& root : mapRoots)
                for (const auto& path : files::RGlob(root / "maps", ".gsc"))
                    ReadIncludedWeapons(paths::ReadSource(path), names);
            return names;
        }

        void AddLevelWeapons(const Names& named, Names& weapons, const Json& levelWeapons, std::vector<std::string>& warnings)
        {
            Names common;
            for (const auto& name : levelWeapons)
                common.insert(mod::Lower(name.get<std::string>()));
            for (const auto& name : named)
            {
                if (common.contains(mod::Lower(name)) && weapons.insert(name).second)
                {
                    warnings.push_back("WAW_STOCK_ASSET weapon " + name + ": the map names it; WaW loads it with every level from ['common']");
                }
            }
        }
    }

    Inventory BridgeInventory(const std::vector<fs::path>& roots, const std::vector<fs::path>& mapRoots,
                              const std::optional<fs::path>& entities, const Json& levelWeapons, std::vector<std::string>& warnings)
    {
        Names compiled;
        for (const auto& source : weapons::Discover(roots))
            compiled.insert(source.name);
        if (entities && !levelWeapons.empty())
            AddLevelWeapons(NamedWeapons(*entities, mapRoots), compiled, levelWeapons, warnings);
        // A zone defines whole alias tables. The sound stage decides which
        // candidates actually play by following script and asset references.
        return {std::move(compiled), SoundAliases(roots), ShippedModels(mapRoots)};
    }
}
