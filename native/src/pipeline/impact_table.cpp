// WaW's impact effects (see impact_table.h).
#include "impact_table.h"

#include "core/files.h"
#include "extract/stockwaw.h"
#include "modutil.h"
#include "oat/waw_tables.h"
#include "source_zones.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;
    using util::Json;

    namespace
    {
        constexpr std::size_t T4_SURFACES = 31, T6_SURFACES = 32, FLESH = 4; // BO2 adds riotshield

        // t4sp.exe 0x83f1e0
        constexpr std::array<const char*, 16> T4_ROWS{"bullet_small_normal", "bullet_small_exit", "bullet_small_underwater", "bullet_large_normal",
                                                      "bullet_large_exit",   "shotgun_normal",    "shotgun_exit",            "bullet_ap_normal",
                                                      "bullet_ap_exit",      "grenade_bounce",    "grenade_explode",         "rocket_explode",
                                                      "projectile_dud",      "mortar_shell",      "tank_shell",              "physics_impact"};
        // t6zm.exe 0xc62318, with the WaW row that plays where WaW has no such hit
        constexpr std::array<std::pair<const char*, const char*>, 21> T6_ROWS{{{"bullet_small_normal", ""},
                                                                               {"bullet_small_exit", ""},
                                                                               {"bullet_small_underwater", ""},
                                                                               {"bullet_large_normal", ""},
                                                                               {"bullet_large_exit", ""},
                                                                               {"shotgun_normal", ""},
                                                                               {"shotgun_exit", ""},
                                                                               {"bullet_ap_normal", ""},
                                                                               {"bullet_ap_exit", ""},
                                                                               {"bullet_xtreme_normal", "bullet_ap_normal"},
                                                                               {"bullet_xtreme_exit", "bullet_ap_exit"},
                                                                               {"grenade_bounce", ""},
                                                                               {"grenade_explode", ""},
                                                                               {"rocket_explode", ""},
                                                                               {"rocket_explode_xtreme", "rocket_explode"},
                                                                               {"projectile_dud", ""},
                                                                               {"mortar_shell", ""},
                                                                               {"tank_shell", ""},
                                                                               {"physics_impact", ""},
                                                                               {"bolt", "bullet_small_normal"},
                                                                               {"blade", "bullet_small_normal"}}};

        // the effects both engines play by name: WaW's (t4sp.exe 0x6621b0), BO2's (t6zm.exe 0x7a9060)
        constexpr std::array<std::pair<const char*, const char*>, 6> ENGINE_EFFECTS{
            {{"impacts/flesh_hit_noblood", "impacts/fx_flesh_hit_noblood"},
             {"impacts/flesh_hit_knife", "impacts/fx_flesh_hit_knife_mp"},
             {"impacts/flesh_hit_knife_noblood", "impacts/fx_flesh_hit_knife_noblood"},
             {"system_elements/fx_blood_drops_decal_emit", "system_elements/fx_blood_drops_decal_emit"},
             {"bio/player/fx_player_water_waist_ripple", "bio/player/fx_player_water_waist_ripple"},
             {"bio/player/fx_player_water_knee_ripple", "bio/player/fx_player_water_knee_ripple"}}};

        constexpr const char* TABLE = "waw_impacts";
    } // namespace

    ImpactTable ReadImpacts(const std::vector<fs::path>& roots, const extract::StockWaw* stock)
    {
        ImpactTable out;
        auto zones = SourceZones(roots);
        // WaW loads a map's patch over the map, and the map over the mod
        const auto order = [](const fs::path& zone)
        {
            const auto stem = mod::Lower(files::Utf8(zone.stem()));
            return stem.ends_with("_patch") ? 0 : stem == "mod" ? 2 : 1;
        };
        std::ranges::stable_sort(zones, {}, order);
        if (stock)
            zones.push_back(stock->ZoneDir() / "common.ff");
        for (const auto& zone : zones)
        {
            if (!mod::IsFile(zone))
                continue;
            const auto table = Json::parse(oat::ReadImpactTable({zone, {}}));
            if (table.is_null())
                continue;
            out.zone = files::Utf8(zone.filename());
            for (const auto& row : table)
            {
                auto& names = out.rows.emplace_back();
                for (const auto& name : row)
                {
                    names.push_back(name.get<std::string>());
                    if (!names.back().empty())
                        out.effects.insert(names.back());
                }
            }
            break;
        }
        if (!out.rows.empty())
            for (const auto& [t4, t6] : ENGINE_EFFECTS)
                out.effects.insert(t4);
        return out;
    }

    std::vector<std::vector<std::string>> T6Impacts(const std::vector<std::vector<std::string>>& rows, const std::map<std::string, std::string>& converted)
    {
        if (rows.size() != T4_ROWS.size())
            throw std::runtime_error("a WaW impact table has " + std::to_string(T4_ROWS.size()) + " rows, not " + std::to_string(rows.size()));
        const auto name = [&](const std::string& effect)
        {
            const auto found = converted.find(effect);
            return found == converted.end() ? std::string() : found->second;
        };
        std::vector<std::vector<std::string>> out;
        for (const auto& [row, stand] : T6_ROWS)
        {
            const std::string wanted = *stand ? stand : row;
            const auto index = static_cast<std::size_t>(std::ranges::find(T4_ROWS, wanted) - T4_ROWS.begin());
            const auto& source = rows.at(index);
            auto& target = out.emplace_back(T6_SURFACES + FLESH);
            for (std::size_t i = 0; i < T4_SURFACES; i++)
                target[i] = name(source.at(i));
            for (std::size_t i = 0; i < FLESH; i++)
                target[T6_SURFACES + i] = name(source.at(T4_SURFACES + i));
        }
        return out;
    }

    StagedImpacts StageImpacts(const fs::path& project, const ImpactTable& impacts, const std::map<std::string, std::string>& converted)
    {
        StagedImpacts out;
        if (impacts.rows.empty())
            return out;
        for (const auto& effect : impacts.effects)
            if (!converted.contains(effect))
                out.missing.insert(effect);
        files::WriteText(project / "fx" / (std::string(TABLE) + ".w2bimpacts.json"), util::JsonDumps(Json(T6Impacts(impacts.rows, converted)), 1) + "\n");
        out.table.emplace_back(TABLE);
        for (const auto& [t4, t6] : ENGINE_EFFECTS)
            if (const auto found = converted.find(t4); found != converted.end())
            {
                const auto source = project / "fx" / files::FromUtf8(found->second + ".w2bfx.json");
                if (!mod::IsFile(source))
                    continue;
                files::WriteText(project / "fx" / (std::string(t6) + ".w2bfx.json"), files::ReadText(source));
                out.fx.emplace_back(t6);
            }
        return out;
    }
} // namespace w2b::pipeline
