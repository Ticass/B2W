#include "weapons/plan.h"

#include "core/files.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_set>

namespace w2b::weapons
{
    namespace
    {
        constexpr std::string_view weaponSignature = "WEAPONFILE\\";
        constexpr std::string_view soundSuffix = ".w2bsnd.json";

        bool IsWeapon(const path& file)
        {
            if (!files::fs::is_regular_file(file))
                return false;
            std::ifstream input(file, std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot read " + files::Utf8(file));
            std::array<char, weaponSignature.size()> bytes{};
            return input.read(bytes.data(), bytes.size()) && std::string_view(bytes.data(), bytes.size()) == weaponSignature;
        }

        Names FindSoundAliases(const std::vector<path>& roots)
        {
            Names names;
            for (const auto& root : roots)
            {
                const auto folder = root / "soundaliases";
                for (const auto& file : files::GlobSuffix(folder, soundSuffix))
                {
                    auto name = PosixPath(file.lexically_relative(folder));
                    if (name.ends_with(soundSuffix))
                        name.resize(name.size() - soundSuffix.size());
                    names.insert(std::move(name));
                }
            }
            return names;
        }

        void AddBounceSounds(Weapon& weapon, const Names& aliases)
        {
            std::string_view name = Field(weapon.fields, "bounceSound");
            if (name.starts_with("waw/"))
                name.remove_prefix(4);
            if (name.empty())
                return;
            const auto prefix = std::string(name) + "_";
            weapon.AddDependency("sound", prefix + "default");
            for (auto it = aliases.lower_bound(prefix); it != aliases.end() && it->starts_with(prefix); ++it)
                weapon.AddDependency("sound", *it);
        }
    }

    std::vector<WeaponSource> Discover(const std::vector<path>& roots)
    {
        std::vector<WeaponSource> sources;
        std::unordered_set<std::string> seen;
        for (const auto& root : roots)
        {
            const auto directory = root / "weapons";
            for (const auto& file : files::GlobSuffix(directory, ""))
            {
                auto name = PosixPath(file.lexically_relative(directory));
                if (!seen.contains(name) && IsWeapon(file))
                {
                    seen.insert(name);
                    sources.push_back({std::move(name), file});
                }
            }
        }
        return sources;
    }

    Plan PlanWeapons(const std::vector<path>& roots, const path& project, const path& base)
    {
        const auto t4 = ReadSchema(base / "vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon/WeaponFields.h");
        const auto t6 = ReadSchema(base / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/WeaponFields.h");
        const auto t6Enums = ReadEnumValues(base / "vendor/OpenAssetToolsT6/src/ObjLoading/Game/T6/Weapon/WeaponInfoStringLoaderT6.cpp",
                                            base / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/WeaponStrings.h");
        Plan plan;
        plan.sources = Discover(roots);
        for (const auto& source : plan.sources)
            plan.weapons.push_back(Convert(source.name, ReadInfo(source.file), t4, t6, &t6Enums));
        if (std::ranges::any_of(plan.weapons, [](const Weapon& weapon) { return !Field(weapon.fields, "bounceSound").empty(); }))
        {
            const auto aliases = FindSoundAliases(roots);
            for (auto& weapon : plan.weapons)
                AddBounceSounds(weapon, aliases);
        }
        std::vector<std::size_t> ids(plan.sources.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
            ids[i] = i;
        std::ranges::sort(ids, {}, [&](std::size_t i) { return plan.sources[i].name; });
        auto entries = Json::array();
        for (const auto i : ids)
        {
            auto entry = plan.weapons[i].Report();
            entry["source"] = files::Utf8(plan.sources[i].file);
            entries.push_back(std::move(entry));
        }
        plan.report = {{"weapons", std::move(entries)}};
        files::WriteText(project / "weapons.plan.json", util::JsonDumps(plan.report, 2) + "\n");
        return plan;
    }
}
