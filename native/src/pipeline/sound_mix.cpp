// The sound mix measurements and the bank (see sound_mix.h).
#include "sound_mix.h"

#include "core/profile.h"
#include "core/timeline.h"

#include "core/encoding.h"
#include "core/pyre.h"
#include "core/sum.h"
#include "csvtable.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "gsc/parse.h"
#include "gsc/port/api.h"
#include "modutil.h"
#include "sound/bank.h"
#include "amb_stage.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <cmath>
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
        const std::vector<std::pair<std::string, std::string>> STOCK_FAMILY{{"music", "mus_"}, {"announcer", "vox_zmba_"}, {"ambience", "amb_"}};
        const std::vector<std::string> MIX_COLUMNS{"Bus", "VolumeGroup", "DuckGroup", "Duck", "PanType", "IsMusic", "ReverbSend", "CenterSend"};
        const std::vector<std::string> TRANSFER_COLUMNS{"DistMin", "Falloff", "PriorityMin", "PriorityMax", "OcclusionLevel"};
        const std::vector<std::string> TWIN_COLUMNS{"Bus", "VolumeGroup", "DuckGroup", "ReverbSend", "CenterSend", "DistMin", "DistMaxDry", "DistMaxWet",
                                                    "DryMinCurve", "DryMaxCurve", "WetMinCurve", "WetMaxCurve", "LimitCount", "EntityLimitCount",
                                                    "LimitType", "EntityLimitType", "PriorityMin", "PriorityMax", "PriorityThresholdMin",
                                                    "PriorityThresholdMax", "PanType", "Pan", "OcclusionLevel", "DistanceLpf", "StopOnEntDeath"};
        const std::vector<std::string> REVERB_ROLES{"grp_weapon", "grp_voice", "grp_ambience", "grp_music", "grp_menu"};
        const std::vector<std::string> FRAMEWORK_WEAPON_SOUNDS{"wpn_melee_whoosh_plr", "wpn_melee_whoosh_npc", "wpn_melee_knife_hit_body",
                                                               "wpn_melee_hit_other", "zmb_bowie_swing", "zmb_bowie_swing_plr"};
        const std::vector<std::string> ALWAYS_LOADED_BANKS{"cmn_", "zmb_common.", "zmb_patch", "zmb_code_post_gfx"};
        constexpr int MIN_GROUP_ROWS = 100, REFERENCE_MIN_SAMPLES = 20, REVERB_MIN_SAMPLES = 20;
        constexpr double REVERB_SEND_DB = -24.0, CURVE_TOLERANCE = 1e-5;
        constexpr long long T6_SND_ENTRY_POOL = 0x4000;
        const long long LOADED_BYTES_MEASURED = static_cast<long long>(157.6 * 1048576) + 34LL * 1048576;
        const std::vector<std::pair<std::string, std::vector<std::string>>> ENGINE_SOUND_FAMILIES{
            {"fly_step_(walk|run|sprint)_plr_(?P<s>[a-z]+)(_swt)?", {"step_\\1_plr_\\g<s>", "step_\\1_\\g<s>"}},
            {"fly_step_(walk|run|sprint)(_npc)?_(?P<s>[a-z]+)", {"step_\\1_\\g<s>"}},
            {"fly_step_crouch_(walk|run)_plr_(?P<s>[a-z]+)", {"step_\\1_plr_\\g<s>", "step_\\1_\\g<s>"}},
            {"fly_step_crouch_(walk|run)(_npc)?_(?P<s>[a-z]+)", {"step_\\1_\\g<s>"}},
            {"fly_step_prone_plr_(?P<s>[a-z]+)", {"step_prone_plr_\\g<s>", "step_prone_\\g<s>"}},
            {"fly_step_prone_npc_(?P<s>[a-z]+)", {"step_prone_\\g<s>"}},
            {"fly_land_damage_plr", {"land_damage_plr", "land_damage"}},
            {"fly_land_damage_npc", {"land_damage"}},
            {"fly_land_plr_(?P<s>[a-z]+)(_big)?(_sec)?", {"land_plr_\\g<s>", "land_\\g<s>"}},
            {"fly_land_npc_(?P<s>[a-z]+)(_big)?", {"land_\\g<s>"}},
            {"fly_bodyfall_(large|small)_(?P<s>[a-z]+)", {"bodyfall_\\g<s>_\\1"}},
            {"prj_bullet_impact_(small|large|ap)_(?P<s>[a-z]+)", {"bullet_\\1_\\g<s>"}},
            {"prj_bullet_impact_xtreme_(?P<s>[a-z]+)", {"bullet_large_\\g<s>"}},
            {"prj_bullet_impact_(small|large)_exit_(?P<s>[a-z]+)", {"bullet_\\1_exit_\\g<s>"}},
            {"prj_bullet_impact_(ap|xtreme)_exit_(?P<s>[a-z]+)", {"bullet_large_exit_\\g<s>"}},
            {"prj_bulletspray_small_exit_(?P<s>[a-z]+)", {"bulletspray_small_exit_\\g<s>"}},
            {"wpn_grenade_bounce_(?P<s>[a-z]+)", {"grenade_bounce_\\g<s>"}},
            {"wpn_grenade_explode_(?P<s>[a-z]+)", {"grenade_explode_\\g<s>"}},
            {"wpn_sticky_gren_bounce_(?P<s>[a-z]+)", {"sticky_gren_bounce_\\g<s>"}},
            {"zmb_monkey_land_(?P<s>[a-z]+)", {"monkey_land_\\g<s>"}},
            {"wpn_melee_hit", {"melee_hit"}},
            {"wpn_melee_knife_hit_(body|other)", {"melee_knife_hit_\\1"}},
            {"wpn_melee_hit_other", {"melee_knife_hit_other"}},
            {"wpn_melee_whoosh_plr|zmb_melee_whoosh_plr", {"melee_swing_plr"}},
            {"wpn_melee_whoosh_npc", {"melee_swing"}},
            {"wpn_knife_melee_stab", {"knife_stab_plr"}},
            {"zmb_bowie_(swing)_plr", {"bowie_\\1"}},
            {"zmb_bowie_(swing|stab|pull)", {"bowie_\\1"}},
            {"zmb_bowie_flourish_(catch|start|toss|turn)", {"bowie_\\1"}},
            {"wpn_knife_pull_plr", {"knife_pull_plr"}}};

        // csv.DictReader rows: header -> value ("" when the row is short)
        using Row = std::map<std::string, std::string>;

        std::vector<Row> DictRows(const fs::path& path)
        {
            const auto rows = ReadCsv(encoding::Utf8Replace(mod::Bytes(path)));
            std::vector<Row> out;
            if (rows.empty())
                return out;
            const auto& header = rows[0];
            for (size_t i = 1; i < rows.size(); i++)
            {
                if (rows[i].empty())
                    continue;
                Row row;
                for (size_t c = 0; c < header.size(); c++)
                    row[header[c]] = c < rows[i].size() ? rows[i][c] : std::string();
                out.push_back(std::move(row));
            }
            return out;
        }

        std::string Get(const Row& row, const std::string& key)
        {
            const auto it = row.find(key);
            return it == row.end() ? std::string() : it->second;
        }

        std::optional<double> Number(const std::string& text)
        {
            double v = 0;
            return py::ParseFloat(text, v) ? std::optional<double>(v) : std::nullopt;
        }

        double Median(std::vector<double> values)
        {
            std::sort(values.begin(), values.end());
            const auto middle = values.size() / 2;
            return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
        }

        std::vector<double> Percentiles(std::vector<double> values)
        {
            std::sort(values.begin(), values.end());
            std::vector<double> out;
            for (int i = 0; i <= 100; i++)
                out.push_back(values[static_cast<size_t>(std::nearbyint(i * (static_cast<double>(values.size()) - 1) / 100))]);
            return out;
        }

        double RoundTo(const double v, const int digits)
        {
            char text[64];
            std::snprintf(text, sizeof text, "%.*f", digits, v);
            return std::strtod(text, nullptr);
        }

        std::string G(const char* format, const double v)
        {
            char text[64];
            std::snprintf(text, sizeof text, format, v);
            return text;
        }

        // Counter.most_common(1)[0]: the highest count, the first seen on a tie
        template <typename K> struct Counter
        {
            std::vector<std::pair<K, long long>> items;
            void Add(const K& key)
            {
                for (auto& [k, n] : items)
                    if (k == key)
                    {
                        n++;
                        return;
                    }
                items.emplace_back(key, 1);
            }
            std::pair<K, long long> MostCommon() const
            {
                auto best = items.front();
                for (const auto& item : items)
                    if (item.second > best.second)
                        best = item;
                return best;
            }
            bool Empty() const
            {
                return items.empty();
            }
        };

        using Points = std::vector<std::vector<double>>;

        double CurveArea(const Points& p)
        {
            // sum() of floats: Python compensates (Neumaier)
            std::vector<double> terms;
            for (size_t i = 0; i + 1 < p.size(); i++)
                terms.push_back((p[i + 1][0] - p[i][0]) * (p[i][1] + p[i + 1][1]) / 2);
            return py::Sum(terms);
        }

        bool IsFalloff(const Points& p)
        {
            if (std::abs(p[0][1] - 1) > CURVE_TOLERANCE)
                return false;
            for (size_t i = 0; i + 1 < p.size(); i++)
                if (p[i + 1][1] > p[i][1] + CURVE_TOLERANCE)
                    return false;
            return true;
        }

        bool IsAllOn(const Points& p)
        {
            for (size_t i = 0; i + 1 < p.size(); i++)
                if (std::abs(p[i][1] - 1) > CURVE_TOLERANCE)
                    return false;
            return true;
        }

        std::pair<bool, double> FalloffDescriptor(const double distMin, const double distMax, const Points& minPoints, const Points& maxPoints)
        {
            return {IsAllOn(minPoints) && IsFalloff(maxPoints), std::max(distMax - distMin, 0.0) * CurveArea(maxPoints)};
        }

        // sounds._category: the BO2 volume group of a WaW bus
        std::string CategoryGroup(const std::string& busName, const Json& busGroups)
        {
            const auto bus = Lower(busName);
            if (busGroups.is_object() && busGroups.contains(bus) && busGroups[bus].is_string() && !busGroups[bus].get<std::string>().empty())
                return busGroups[bus].get<std::string>();
            const auto has = [&](std::initializer_list<const char*> keys) { return std::any_of(keys.begin(), keys.end(), [&](const char* k) { return bus.find(k) != std::string::npos; }); };
            if (bus.find("music") != std::string::npos || bus.substr(0, 3) == "mus")
                return "grp_music";
            if (has({"voice", "vox", "dialog"}))
                return "grp_voice";
            if (has({"ui", "menu", "hud"}))
                return "grp_menu";
            if (has({"amb", "element"}))
                return "grp_ambience";
            if (has({"wpn", "weap", "rfl", "pis", "smg", "mg_", "shot", "explo"}))
                return "grp_weapon";
            return "grp_foley";
        }

        class Stock
        {
        public:
            explicit Stock(fs::path bo2) : bo2_(std::move(bo2))
            {
            }

            // _stock_rows: every row of BO2's stock zombie alias tables
            const std::vector<Row>& Rows()
            {
                if (!loaded_)
                {
                    loaded_ = true;
                    for (const auto& path : Glob(bo2_ / "raw/soundbank", "zmb_", ".aliases.csv"))
                    {
                        auto rows = DictRows(path);
                        rows_.insert(rows_.end(), rows.begin(), rows.end());
                    }
                }
                return rows_;
            }

            static std::vector<fs::path> Glob(const fs::path& folder, const std::string& prefix, const std::string& suffix)
            {
                std::vector<fs::path> out;
                std::error_code e;
                if (!IsDir(folder))
                    return out;
                for (const auto& entry : fs::directory_iterator(folder, e))
                {
                    const auto name = Lower(files::Utf8(entry.path().filename()));
                    if (StartsWith(name, prefix) && mod::EndsWith(name, suffix) && name.size() >= prefix.size() + suffix.size())
                        out.push_back(entry.path());
                }
                std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
                return out;
            }

            const fs::path& Root() const
            {
                return bo2_;
            }

        private:
            fs::path bo2_;
            bool loaded_ = false;
            std::vector<Row> rows_;
        };

        std::map<std::string, Points> ReadCurves(const fs::path& path)
        {
            std::map<std::string, Points> out;
            if (!IsFile(path))
                return out;
            for (const auto& row : DictRows(path))
            {
                Points p;
                for (int i = 0; i < 8; i++)
                    p.push_back({*Number(Get(row, "x" + std::to_string(i))), *Number(Get(row, "y" + std::to_string(i)))});
                out[Get(row, "name")] = p;
            }
            return out;
        }

        Json StockClasses(Stock& stock)
        {
            const auto curves = ReadCurves(stock.Root() / "raw/soundbank/globals/curves.csv");
            std::map<std::string, std::vector<const Row*>> found;
            for (const auto& row : stock.Rows())
                found[Get(row, "VolumeGroup") + "|" + Get(row, "PanType")].push_back(&row);
            Json classes = Json::object();
            for (const auto& [key, rows] : found)
            {
                if (static_cast<int>(rows.size()) < MIN_GROUP_ROWS)
                    continue;
                std::map<std::string, std::vector<double>> values;
                for (const auto& c : TRANSFER_COLUMNS)
                    values[c];
                Counter<std::string> dry, wet;
                for (const auto* row : rows)
                {
                    for (const char* column : {"PriorityMin", "PriorityMax", "OcclusionLevel"})
                        if (const auto v = Number(Get(*row, column)))
                            values[column].push_back(*v);
                    const auto low = curves.find(Get(*row, "DryMinCurve")), high = curves.find(Get(*row, "DryMaxCurve"));
                    const auto nearV = Number(Get(*row, "DistMin")), farV = Number(Get(*row, "DistMaxDry"));
                    if (Get(*row, "PanType") != "3d" || low == curves.end() || high == curves.end() || !nearV || !farV)
                        continue;
                    const auto [standard, falloff] = FalloffDescriptor(*nearV, *farV, low->second, high->second);
                    if (standard)
                    {
                        values["DistMin"].push_back(*nearV);
                        values["Falloff"].push_back(falloff);
                        dry.Add(Get(*row, "DryMaxCurve"));
                        const auto wetCurve = curves.find(Get(*row, "WetMaxCurve"));
                        if (IsFalloff(wetCurve != curves.end() ? wetCurve->second : Points{{0, 0}}))
                            wet.Add(Get(*row, "WetMaxCurve"));
                    }
                }
                Counter<std::pair<std::string, std::string>> routing;
                for (const auto* row : rows)
                    routing.Add({Get(*row, "Bus"), Get(*row, "DuckGroup")});
                const auto route = routing.MostCommon().first;
                Json quantiles = Json::object();
                for (const auto& c : TRANSFER_COLUMNS)
                    if (static_cast<int>(values[c].size()) >= MIN_GROUP_ROWS)
                        quantiles[c] = Percentiles(values[c]);
                Json entry{{"rows", rows.size()}, {"routing", Json{{"Bus", route.first}, {"DuckGroup", route.second}}}, {"quantiles", quantiles}};
                if (!dry.Empty())
                {
                    const auto curve = dry.MostCommon().first;
                    std::vector<std::string> falloffCurves;
                    for (const auto& [n, p] : curves)
                        if (IsFalloff(p))
                            falloffCurves.push_back(n);
                    entry["falloff"] = Json{{"DryMaxCurve", curve}, {"area", CurveArea(curves.at(curve))}, {"WetMaxCurve", wet.Empty() ? curve : wet.MostCommon().first},
                                            {"falloff_curves", falloffCurves}};
                }
                classes[key] = entry;
            }
            return classes;
        }

        Json StockTwins(Stock& stock, const std::vector<std::string>& names)
        {
            std::set<std::string> wanted;
            for (const auto& n : names)
                wanted.insert(Lower(n));
            std::vector<std::pair<std::string, Counter<std::vector<std::string>>>> found;
            if (!wanted.empty())
                for (const auto& row : stock.Rows())
                {
                    const auto name = Lower(Get(row, "Name"));
                    if (!wanted.count(name))
                        continue;
                    std::vector<std::string> mix;
                    for (const auto& c : TWIN_COLUMNS)
                        mix.push_back(Get(row, c));
                    auto it = std::find_if(found.begin(), found.end(), [&](const auto& f) { return f.first == name; });
                    if (it == found.end())
                        it = found.insert(found.end(), {name, {}});
                    it->second.Add(mix);
                }
            Json out = Json::object();
            for (const auto& [name, mixes] : found)
            {
                const auto mix = mixes.MostCommon().first;
                Json row = Json::object();
                for (size_t i = 0; i < TWIN_COLUMNS.size(); i++)
                    row[TWIN_COLUMNS[i]] = mix[i];
                out[name] = row;
            }
            return out;
        }

        std::vector<std::pair<std::string, std::string>> StockAliasGroups(Stock& stock)
        {
            std::vector<std::pair<std::string, Counter<std::string>>> found;
            std::map<std::string, size_t> index;
            for (const auto& row : stock.Rows())
            {
                const auto name = Lower(Get(row, "Name"));
                auto it = index.find(name);
                if (it == index.end())
                {
                    it = index.emplace(name, found.size()).first;
                    found.push_back({name, {}});
                }
                found[it->second].second.Add(Get(row, "VolumeGroup"));
            }
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& [name, groups] : found)
                out.emplace_back(name, groups.MostCommon().first);
            return out;
        }

        Json StockProfiles(Stock& stock, const std::vector<std::string>& stingers, const std::vector<std::string>& twins)
        {
            std::map<std::string, std::vector<const Row*>> rows;
            for (const auto& [role, _] : STOCK_FAMILY)
                rows[role];
            for (const auto& row : stock.Rows())
            {
                const auto name = Lower(Get(row, "Name"));
                for (const auto& [role, prefix] : STOCK_FAMILY)
                    if (StartsWith(name, prefix))
                        rows[role].push_back(&row);
            }
            Json profiles = Json::object();
            const auto mixOf = [](const Row& r, const std::vector<std::string>& columns)
            {
                std::vector<std::string> mix;
                for (const auto& c : columns)
                    mix.push_back(Get(r, c));
                return mix;
            };
            for (const auto& [role, _] : STOCK_FAMILY)
            {
                const auto& all = rows[role];
                if (all.empty())
                    continue;
                Counter<std::vector<std::string>> heads;
                for (const auto* r : all)
                    heads.Add({Get(*r, "Bus"), Get(*r, "VolumeGroup"), Get(*r, "PanType")});
                const auto head = heads.MostCommon().first;
                std::vector<const Row*> found;
                for (const auto* r : all)
                    if (std::vector<std::string>{Get(*r, "Bus"), Get(*r, "VolumeGroup"), Get(*r, "PanType")} == head)
                        found.push_back(r);
                Counter<std::vector<std::string>> mixes;
                for (const auto* r : found)
                    mixes.Add(mixOf(*r, MIX_COLUMNS));
                const auto [mix, count] = mixes.MostCommon();
                std::vector<double> volumes;
                for (const auto* r : found)
                    if (mixOf(*r, MIX_COLUMNS) == mix)
                        if (const auto v = Number(Get(*r, "VolMax")))
                            volumes.push_back(*v);
                Json mixJson = Json::object();
                for (size_t i = 0; i < MIX_COLUMNS.size(); i++)
                    mixJson[MIX_COLUMNS[i]] = mix[i];
                profiles[role] = Json{{"mix", mixJson}, {"volume", volumes.empty() ? Json() : Json(Median(volumes))}, {"rows", count}};
            }
            static const std::vector<std::string> STINGER_COLUMNS = []
            {
                auto c = MIX_COLUMNS;
                c.push_back("Pan");
                return c;
            }();
            for (const auto& name : stingers)
            {
                // _stinger_profile: the most common mix, the loudest map's level
                std::vector<std::vector<Row>> perMap;
                std::vector<const Row*> found;
                for (const auto& path : Stock::Glob(stock.Root() / "raw/soundbank", "zmb_", ".aliases.csv"))
                {
                    std::vector<Row> group;
                    for (auto& r : DictRows(path))
                        if (Lower(Get(r, "Name")) == name)
                            group.push_back(std::move(r));
                    perMap.push_back(std::move(group));
                }
                for (const auto& g : perMap)
                    for (const auto& r : g)
                        found.push_back(&r);
                if (found.empty())
                    continue;
                Counter<std::vector<std::string>> mixes;
                for (const auto* r : found)
                    mixes.Add(mixOf(*r, STINGER_COLUMNS));
                const auto mix = mixes.MostCommon().first;
                std::vector<double> levels;
                for (const auto& g : perMap)
                {
                    std::vector<double> v;
                    for (const auto& r : g)
                        if (const auto x = Number(Get(r, "VolMax")))
                            v.push_back(*x);
                    if (!v.empty())
                        levels.push_back(Median(v));
                }
                Json mixJson = Json::object();
                for (size_t i = 0; i < STINGER_COLUMNS.size(); i++)
                    mixJson[STINGER_COLUMNS[i]] = mix[i];
                profiles["stinger:" + name] = Json{{"mix", mixJson}, {"volume", levels.empty() ? Json() : Json(*std::max_element(levels.begin(), levels.end()))},
                                                   {"rows", found.size()}};
            }
            if (!rows["ambience"].empty())
            {
                // stock_emitters: BO2's 3D ambient emitter placement per looping value
                Json emitters = Json::object();
                for (const char* looping : {"looping", "nonlooping"})
                {
                    std::vector<const Row*> found;
                    for (const auto* r : rows["ambience"])
                        if (Get(*r, "PanType") == "3d" && Get(*r, "Looping") == looping)
                            found.push_back(r);
                    if (found.empty())
                        continue;
                    Json columns = Json::object();
                    for (const char* c : {"DistMin", "DistMaxDry", "DistMaxWet"})
                    {
                        std::vector<double> v;
                        for (const auto* r : found)
                            if (const auto x = Number(Get(*r, c)))
                                v.push_back(*x);
                        columns[c] = G("%g", Median(v));
                    }
                    for (const char* c : {"DryMinCurve", "DryMaxCurve", "WetMinCurve", "WetMaxCurve"})
                    {
                        Counter<std::string> counts;
                        for (const auto* r : found)
                            counts.Add(Get(*r, c));
                        columns[c] = counts.MostCommon().first;
                    }
                    emitters[looping] = Json{{"columns", columns}, {"rows", found.size()}};
                }
                profiles["ambience"]["emitters"] = emitters;
            }
            {
                // stock_group_levels: median stock VolMax per class
                std::vector<std::pair<std::string, std::vector<double>>> values;
                std::map<std::string, size_t> index;
                for (const auto& row : stock.Rows())
                    if (const auto v = Number(Get(row, "VolMax")))
                    {
                        const auto key = Get(row, "VolumeGroup") + "|" + Get(row, "PanType");
                        auto it = index.find(key);
                        if (it == index.end())
                        {
                            it = index.emplace(key, values.size()).first;
                            values.push_back({key, {}});
                        }
                        values[it->second].second.push_back(*v);
                    }
                Json groups = Json::object();
                for (const auto& [key, found] : values)
                    if (static_cast<int>(found.size()) >= MIN_GROUP_ROWS && Median(found) > 0)
                        groups[key] = Json{{"volume", Median(found)}, {"rows", found.size()}};
                profiles["groups"] = groups;
            }
            profiles["classes"] = StockClasses(stock);
            profiles["twins"] = StockTwins(stock, twins);
            return profiles;
        }

        Json Indexed(const Json& globals, const char* table, const long long index)
        {
            std::vector<const Json*> hits;
            for (const auto& row : globals[table])
                if (row["index"] == index)
                    hits.push_back(&row);
            if (hits.size() != 1 || !hits[0]->contains("name") || !(*hits[0])["name"].is_string() || (*hits[0])["name"].get<std::string>().empty())
                throw std::runtime_error("undefined T4 slot");
            return *hits[0];
        }

        Points CurvePoints(const Json& curve)
        {
            const auto& points = curve["points"];
            const size_t count = curve.contains("pointCount") ? curve["pointCount"].get<size_t>() : points.size();
            Points out;
            for (size_t i = 0; i < std::min(count, points.size()); i++)
                out.push_back({points[i][0].get<double>(), points[i][1].get<double>()});
            bool bad = out.size() < 2 || out.front()[0] != 0 || out.back()[0] != 1;
            for (size_t i = 1; i < out.size() && !bad; i++)
                bad = !std::isfinite(out[i][0]) || !std::isfinite(out[i][1]) || out[i][0] < out[i - 1][0];
            if (bad)
                throw std::runtime_error("invalid curve");
            return out;
        }

        struct Record
        {
            std::string name, bus;
            bool spatialized, noWetLevel, standard;
            double reverbSend, falloff;
            std::optional<double> volMax;
            double distMin, priorityMin, priorityMax, occlusionLevel;
        };

        // sounds.waw_mix_records: one record per stock WaW alias variant
        std::vector<Record> WawMixRecords(const std::vector<fs::path>& roots, const Json& globals)
        {
            std::vector<Record> records;
            std::set<std::pair<std::string, size_t>> seen;
            for (const auto& root : roots)
            {
                auto paths = files::RGlob(root / "soundaliases", ".w2bsnd.json");
                std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
                for (const auto& path : paths)
                {
                    const auto document = mod::Json(path);
                    const bool named = document.contains("name") && document["name"].is_string();
                    const auto docName = named ? document["name"].get<std::string>() : std::string();
                    const auto key0 = named ? docName : std::string("\x01None");
                    size_t index = 0;
                    const auto aliases = document.value("aliases", Json::array());
                    for (const auto& alias : aliases)
                    {
                        const auto key = std::pair{key0, index++};
                        if (!seen.insert(key).second)
                            continue;
                        const auto flags = alias.at("flags").get<long long>();
                        try
                        {
                            const auto bus = Indexed(globals, "buses", (flags >> 22) & 63);
                            const auto minCurve = CurvePoints(Indexed(globals, "curves", alias.at("volumeMinFalloffCurve").get<long long>()));
                            const auto maxCurve = CurvePoints(Indexed(globals, "curves", alias.at("volumeFalloffCurve").get<long long>()));
                            const double volumeMod = bus.contains("volumeMod") ? bus["volumeMod"].get<double>() : 1.0;
                            const double gain = alias.value("volMax", 0.0) * volumeMod;
                            const auto [standard, falloff] = FalloffDescriptor(alias.value("distMin", 0.0), alias.value("distMax", 0.0), minCurve, maxCurve);
                            records.push_back(Record{Lower(docName), Lower(bus["name"].get<std::string>()), (flags & (1 << 6)) != 0, (flags & (1 << 4)) != 0, standard,
                                                     alias.value("reverbSend", 0.0), falloff,
                                                     gain > 0 && gain <= 1 ? std::optional<double>(std::max(0.0, 100 + 20 * std::log10(gain))) : std::nullopt,
                                                     alias.value("distMin", 0.0), alias.value("minPriority", 0.0), alias.value("maxPriority", 0.0),
                                                     alias.value("occlusionLevel", 0.0)});
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                }
            }
            return records;
        }

        // _bank_entry_sizes: SndAssetBankEntry id -> data size
        // entry id -> size from a bank's entry table (header, then 20-byte entries);
        // reads only those bytes, not the bank's audio
        std::map<uint32_t, uint32_t> BankEntrySizes(const fs::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            uint32_t header[8];
            long long offsets[3];
            if (!stream.read(reinterpret_cast<char*>(header), sizeof header) || !stream.read(reinterpret_cast<char*>(offsets), sizeof offsets))
                throw StepError("SoundError", "unreadable sound bank header: " + Str(path));
            std::string table(static_cast<size_t>(header[5]) * 20, '\0');
            stream.seekg(offsets[1]);
            if (!stream.read(table.data(), static_cast<std::streamsize>(table.size())))
                throw StepError("SoundError", "unreadable sound bank entry table: " + Str(path));
            std::map<uint32_t, uint32_t> sizes;
            for (uint32_t i = 0; i < header[5]; i++)
            {
                uint32_t entry[2];
                std::memcpy(entry, table.data() + 20LL * i, 8);
                sizes[entry[0]] = entry[1];
            }
            return sizes;
        }

        bool AlwaysLoaded(const fs::path& p)
        {
            const auto name = files::Utf8(p.filename());
            return std::any_of(ALWAYS_LOADED_BANKS.begin(), ALWAYS_LOADED_BANKS.end(), [&](const std::string& prefix) { return StartsWith(name, prefix); });
        }

        // sounds.loaded_budget(bo2_root, None)
        sound::SoundBudget LoadedBudget(const fs::path& bo2)
        {
            std::vector<fs::path> banks, all;
            for (const auto& p : Stock::Glob(bo2 / "sound", "", ".all.sabl"))
                if (!AlwaysLoaded(p))
                    banks.push_back(p);
            if (banks.empty())
                throw StepError("SoundError", "no stock BO2 map sound banks under " + Str(bo2 / "sound") + " to measure a budget");
            std::error_code e;
            for (const auto& entry : fs::directory_iterator(bo2 / "sound", e))
            {
                const auto name = Lower(files::Utf8(entry.path().filename()));
                // glob *.sab?
                if (name.size() >= 5 && name.compare(name.size() - 5, 4, ".sab") == 0)
                    all.push_back(entry.path());
            }
            std::sort(all.begin(), all.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
            std::map<std::string, std::map<uint32_t, uint32_t>> stock;
            for (const auto& p : all)
                stock[Str(p)] = BankEntrySizes(p);
            const auto total = [&](const fs::path& p)
            {
                long long sum = 0;
                for (const auto& [_, v] : stock[Str(p)])
                    sum += v;
                return sum;
            };
            fs::path biggest = banks[0];
            for (const auto& b : banks)
                if (total(b) > total(biggest))
                    biggest = b;
            const long long size = total(biggest);
            long long always = 0;
            for (const auto& p : all)
                if (AlwaysLoaded(p))
                    always += static_cast<long long>(stock[Str(p)].size());
            sound::SoundBudget budget;
            budget.referenceBank = files::Utf8(biggest.filename());
            budget.referenceEntries = static_cast<long long>(stock[Str(biggest)].size());
            budget.referenceBytes = size;
            budget.entries = std::max(T6_SND_ENTRY_POOL - always, 0LL);
            budget.entryPool.size = T6_SND_ENTRY_POOL;
            budget.entryPool.alwaysLoaded = always;
            budget.stockHeadroomBytes = std::max(size, 0LL);
            budget.bytes = std::max(std::min(size, LOADED_BYTES_MEASURED), 0LL);
            return budget;
        }

        std::vector<std::string> TemplateAliasNames(const fs::path& csv)
        {
            if (!IsFile(csv))
                return {};
            std::set<std::string> names(FRAMEWORK_WEAPON_SOUNDS.begin(), FRAMEWORK_WEAPON_SOUNDS.end());
            for (const auto& row : DictRows(csv))
                if (!Get(row, "Name").empty())
                    names.insert(Get(row, "Name"));
            return {names.begin(), names.end()};
        }

        Json SoundTableTwins(const fs::path& projectRoot, const fs::path& bo2, const std::string& reg)
        {
            const auto stockFile = bo2 / "raw/maps/mp/zombies/_zm.gsc";
            if (!IsFile(stockFile))
                return Json::object();
            std::map<std::string, std::string> stock;
            for (const auto& m : pyre::Regex(reg + "\\(\\s*\"([^\"]+)\"\\s*,\\s*\"([^\"]+)\"\\s*\\)").FindAll(mod::ReadReplace(stockFile)))
                stock[m[0]] = m[1];
            static const pyre::Regex table("level\\.zombie_sounds\\[\"([^\"]+)\"\\]\\s*=\\s*\"(waw/[^\"]+)\"");
            std::vector<std::pair<std::string, std::set<std::string>>> found;
            auto scripts = files::RGlob(projectRoot / "maps", ".gsc");
            std::sort(scripts.begin(), scripts.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
            for (const auto& script : scripts)
                for (const auto& m : table.FindAll(mod::ReadReplace(script)))
                {
                    const auto it = stock.find(m[0]);
                    if (it == stock.end() || StartsWith(Lower(it->second), "mus_"))
                        continue;
                    const auto alias = Lower(m[1]);
                    auto f = std::find_if(found.begin(), found.end(), [&](const auto& x) { return x.first == alias; });
                    if (f == found.end())
                        f = found.insert(found.end(), {alias, {}});
                    f->second.insert(Lower(it->second));
                }
            Json out = Json::object();
            for (const auto& [alias, twins] : found)
                if (twins.size() == 1)
                    out[alias] = *twins.begin();
            return out;
        }

        std::set<std::string> MusicTableAliases(const std::string& amb)
        {
            static const pyre::Regex call("\\bmusicAlias(?:loop)?\\s*\\(\\s*\"([^\"]+)\"", pyre::IGNORECASE);
            std::set<std::string> out;
            for (const auto& m : call.FindAll(amb))
                out.insert(Lower(m[0]));
            return out;
        }

        Json MusicStingers(const std::string& amb, const Json& states)
        {
            static const pyre::Regex call("\\b(declareMusicState|musicAliasloop|musicAlias)\\s*\\(\\s*\"([^\"]+)\"", pyre::IGNORECASE);
            Json out = Json::object();
            std::optional<std::string> state;
            for (const auto& m : call.FindAll(amb))
            {
                const auto name = Lower(m[0]);
                if (name == "declaremusicstate")
                    state = m[1];
                else if (name == "musicalias" && state && states.contains(*state) && !out.contains(Lower(m[1])))
                    out[Lower(m[1])] = states[*state];
            }
            return out;
        }

        std::optional<std::string> FamilySource(const std::string& name, const std::map<std::string, std::string>& wawBuses)
        {
            for (const auto& [pattern, targets] : ENGINE_SOUND_FAMILIES)
            {
                const pyre::Regex full("\\A(?:" + pattern + ")\\Z");
                if (!full.Search(name))
                    continue;
                for (const auto& t : targets)
                {
                    const auto expanded = full.Sub(t, name, 1);
                    if (const auto it = wawBuses.find(expanded); it != wawBuses.end())
                        return it->second;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::string Plus(const double v)
        {
            return G("%+g", v);
        }
    } // namespace

    SoundMixResult SoundMix(const SoundMixRequest& request)
    {
        const auto& projectRoot = request.projectRoot;
        const auto& project = request.project;
        const auto& bo2Root = request.bo2Root;
        auto* stockWaw = request.stock;
        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        const auto& api = gsc::port::Api();
        Json warnings = Json::array(), content = Json::object();
        const auto bank = "waw_" + project + ".all";
        const std::optional<fs::path> templateCsv = bo2Root ? std::optional(*bo2Root / "mods" / "zm_test" / "soundbank" / "zmb_test.all.aliases.csv") : std::nullopt;
        std::optional<sound::SoundBudget> budget;
        if (bo2Root)
        {
            budget = [&] { const profile::Scope scope("sound mix: loaded budget"); return LoadedBudget(*bo2Root); }();
            std::set<std::string> played;
            played.insert(request.scriptSounds.begin(), request.scriptSounds.end());
            played.insert(request.entitySounds.begin(), request.entitySounds.end());
            for (const auto& [actor, cats] : api["zombie_vocals"].items())
                if (actor != "doc")
                    for (const auto& [_, a] : cats.items())
                        played.insert(a.get<std::string>());
            budget->played = std::vector<std::string>(played.begin(), played.end());
        }
        const auto& audio = api["audio_remaster"];
        // stock_waw_sound_records: every alias of the stock WaW zombies zones
        std::optional<std::vector<Record>> records;
        std::vector<std::string> wawZones;
        std::string recordsReason = "no stock WaW zombies zones or sound driver";
        if (stockWaw)
        {
            const auto driver = stockWaw->RootFor("snddriverglobals", "singleton");
            for (const auto& [zone, _] : stockWaw->Index().items())
                if (StartsWith(zone, "nazi_zombie") && !mod::EndsWith(zone, "_load") && !mod::EndsWith(zone, "_patch"))
                    wawZones.push_back(zone);
            std::sort(wawZones.begin(), wawZones.end());
            if (driver && !wawZones.empty())
            {
                const auto globals = mod::Json(driver->second / "soundglobals/singleton.w2bsndglobals.json");
                std::vector<fs::path> roots;
                for (const auto& zone : wawZones)
                    roots.push_back(stockWaw->Dump(zone, std::string("sound")));
                const timeline::Span span("measure the stock WaW sound mix");
                records = WawMixRecords(roots, globals);
            }
        }
        std::optional<Stock> stock;
        if (bo2Root)
        {
            const timeline::Span span("read the stock BO2 sound tables");
            stock.emplace(*bo2Root);
        }
        // measure_sound_classes: the WaW bus -> BO2 group table of this build
        Json soundClasses;
        if (bo2Root)
        {
            Json busGroups = audio["bus_groups"];
            std::map<std::string, std::string> wawBuses;
            if (records)
                for (const auto& r : *records)
                    wawBuses[r.name] = r.bus;
            std::map<std::string, std::map<std::string, long long>> votes;
            const auto stockGroups = [&] { const profile::Scope scope("sound mix: stock alias groups"); return StockAliasGroups(*stock); }();
            for (const auto& [name, group] : stockGroups)
                if (const auto bus = FamilySource(name, wawBuses))
                    votes[*bus][group]++;
            Json learned = Json::object(), differs = Json::object();
            for (const auto& [bus, found] : votes)
            {
                long long totalVotes = 0;
                for (const auto& [_, n] : found)
                    totalVotes += n;
                if (totalVotes < REVERB_MIN_SAMPLES)
                    continue;
                std::string group;
                long long best = -1;
                for (const auto& [g, n] : found)
                    if (n > best)
                        best = n, group = g;
                learned[bus] = Json{{"group", group}, {"twins", best}, {"of", totalVotes}};
            }
            for (const auto& [bus, vote] : learned.items())
                if (!busGroups.contains(bus) || busGroups[bus] != vote["group"])
                {
                    Json entry{{"declared", busGroups.contains(bus) ? busGroups[bus] : Json()}};
                    entry.update(vote);
                    differs[bus] = entry;
                }
            for (const auto& [bus, vote] : learned.items())
                busGroups[bus] = vote["group"];
            soundClasses = Json{{"bus_groups", busGroups}, {"weapon_dependency_group", audio["weapon_dependency_group"]}, {"learned", learned}, {"differs", differs}};
            std::vector<std::string> busNames;
            for (const auto& [bus, _] : differs.items())
                busNames.push_back(bus);
            std::sort(busNames.begin(), busNames.end());
            for (const auto& bus : busNames)
            {
                const auto& vote = differs[bus];
                warnings.push_back("SOUND_BUS_GROUP WaW bus " + bus + ": stock BO2 mixes its engine sounds in " + vote["group"].get<std::string>() + " (" +
                                   std::to_string(vote["twins"].get<long long>()) + " of " + std::to_string(vote["of"].get<long long>()) + " twins), not the declared " +
                                   (vote["declared"].is_null() ? std::string("None") : vote["declared"].get<std::string>()) + "; the stock group is used");
            }
        }
        // measure_reverb_send: the WaW-to-BO2 send offset from both games' stock aliases
        Json reverbSend;
        {
            const profile::Scope scope("sound mix: reverb send");
            const auto soundbank = bo2Root ? *bo2Root / "raw/soundbank" : fs::path();
            if (!bo2Root || !IsDir(soundbank))
                reverbSend = Json{{"status", "unmeasured_default"}, {"db", REVERB_SEND_DB}, {"reason", "no BO2 mod tools raw/soundbank"}};
            else if (!records)
                reverbSend = Json{{"status", "unmeasured_default"}, {"db", REVERB_SEND_DB}, {"reason", recordsReason}};
            else
            {
                const auto busGroups = !soundClasses.is_null() ? soundClasses["bus_groups"] : Json();
                std::map<std::string, std::vector<double>> waw, bo2;
                for (const auto& r : *records)
                    if (r.reverbSend > 0 && !r.noWetLevel)
                        waw[CategoryGroup(r.bus, busGroups)].push_back(20 * std::log10(r.reverbSend));
                for (const auto& path : Stock::Glob(soundbank, "zmb_", ".all.aliases.csv"))
                    for (const auto& row : DictRows(path))
                    {
                        const auto text = Get(row, "ReverbSend");
                        double send = 0;
                        if (!text.empty() && !py::ParseFloat(text, send))
                            continue;
                        if (send > 0)
                            bo2[Get(row, "VolumeGroup")].push_back(send - 100);
                    }
                std::vector<std::string> groups;
                if (busGroups.is_object() && !busGroups.empty())
                {
                    for (const auto& [g, _] : waw)
                        if (bo2.count(g))
                            groups.push_back(g);
                }
                else
                    groups = REVERB_ROLES;
                Json roles = Json::object();
                std::vector<std::pair<double, long long>> weighted;
                for (const auto& group : groups)
                {
                    const auto a = waw.count(group) ? waw[group] : std::vector<double>{}, b = bo2.count(group) ? bo2[group] : std::vector<double>{};
                    if (static_cast<int>(std::min(a.size(), b.size())) >= REVERB_MIN_SAMPLES)
                    {
                        roles[group] = Json{{"waw_db", Median(a)}, {"bo2_db", Median(b)}, {"offset_db", Median(b) - Median(a)}, {"waw_samples", a.size()}, {"bo2_samples", b.size()}};
                        weighted.emplace_back(Median(b) - Median(a), static_cast<long long>(std::min(a.size(), b.size())));
                    }
                }
                if (roles.empty())
                    reverbSend = Json{{"status", "unmeasured_default"}, {"db", REVERB_SEND_DB}, {"roles", roles}, {"reason", "no role has enough stock sends on both sides"}};
                else
                {
                    std::sort(weighted.begin(), weighted.end());
                    double half = 0, total = 0, offset = 0;
                    for (const auto& [_, w] : weighted)
                        half += static_cast<double>(w);
                    half /= 2;
                    for (const auto& [o, w] : weighted)
                    {
                        total += static_cast<double>(w);
                        offset = o;
                        if (total >= half)
                            break;
                    }
                    reverbSend = Json{{"status", "measured"}, {"db", RoundTo(offset, 2)}, {"roles", roles}, {"waw_zones", wawZones}, {"bo2_soundbank", Str(soundbank)}};
                }
            }
            if (reverbSend["status"] != "measured")
                warnings.push_back("SOUND_REVERB_SEND unmeasured: " + reverbSend["reason"].get<std::string>() + "; using the last stock measurement " + py::FloatRepr(reverbSend["db"].get<double>()) + " dB");
        }
        Json remaster;
        if (bo2Root)
        {
            const auto amb = request.sourceAmb ? encoding::WawText(mod::Bytes(*request.sourceAmb)) : std::string();
            const auto eventsFile = projectRoot / "content_source/announcer_events.json";
            const auto& stingerStates = audio["music_stingers"];
            const auto& twin = audio["vocal_twins"];
            Json vocalTwins = Json::object();
            for (const auto& [actor, cats] : api["zombie_vocals"].items())
                if (actor != "doc")
                    for (const auto& [category, alias] : cats.items())
                    {
                        const auto categoryName = twin["category_names"].contains(category) ? twin["category_names"][category].get<std::string>() : category;
                        auto name = twin["alias"].get<std::string>();
                        for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{{"{actor}", actor}, {"{category}", categoryName}})
                            for (size_t at; (at = name.find(key)) != std::string::npos;)
                                name.replace(at, key.size(), value);
                        vocalTwins[Lower("waw/" + alias.get<std::string>())] = name;
                    }
            const profile::Scope remasterScope("sound mix: remaster profiles");
            auto twins = SoundTableTwins(projectRoot, *bo2Root, api["map_content"]["sound_table"]["register"].get<std::string>());
            for (const auto& [k, v] : vocalTwins.items())
                twins[k] = v;
            const auto engineNames = TemplateAliasNames(*templateCsv);
            if (!records)
                warnings.push_back("AUDIO_MIX " + recordsReason + ": classes are read against this map's own values instead of stock WaW's");
            std::set<std::string> stingerSet;
            for (const auto& [_, v] : stingerStates.items())
                stingerSet.insert(v.get<std::string>());
            std::vector<std::string> twinNames = engineNames;
            for (const auto& [_, v] : twins.items())
                twinNames.push_back(v.get<std::string>());
            Json reference;
            if (records)
            {
                // waw_mix_reference -> reference_quantiles
                // waw_mix_reference keeps first-seen order (dict setdefault)
                using Columns = std::vector<std::pair<std::string, std::vector<double>>>;
                std::vector<std::pair<std::string, Columns>> found;
                const auto add = [](Columns& columns, const std::string& column, const double v)
                {
                    auto it = std::find_if(columns.begin(), columns.end(), [&](const auto& c) { return c.first == column; });
                    if (it == columns.end())
                        it = columns.insert(columns.end(), {column, {}});
                    it->second.push_back(v);
                };
                for (const auto& r : *records)
                {
                    const auto key = CategoryGroup(r.bus, soundClasses["bus_groups"]) + "|" + (r.spatialized ? "3d" : "2d");
                    auto it = std::find_if(found.begin(), found.end(), [&](const auto& f) { return f.first == key; });
                    if (it == found.end())
                        it = found.insert(found.end(), {key, {}});
                    auto& columns = it->second;
                    if (r.volMax)
                        add(columns, "VolMax", *r.volMax);
                    add(columns, "PriorityMin", r.priorityMin);
                    add(columns, "PriorityMax", r.priorityMax);
                    add(columns, "OcclusionLevel", r.occlusionLevel);
                    if (r.spatialized && r.standard)
                    {
                        add(columns, "DistMin", r.distMin);
                        add(columns, "Falloff", r.falloff);
                    }
                }
                reference = Json::object();
                for (const auto& [key, columns] : found)
                {
                    Json out = Json::object();
                    for (const auto& [column, values] : columns)
                        if (static_cast<int>(values.size()) >= REFERENCE_MIN_SAMPLES)
                            out[column] = Json{{"q", Percentiles(values)}, {"samples", values.size()}};
                    reference[key] = out;
                }
            }
            std::set<std::string> ambience;
            for (const auto& a : AmbientAliases(amb))
            {
                auto low = Lower(a);
                ambience.insert(StartsWith(low, "waw/") ? low.substr(4) : low);
            }
            const auto entitiesJson = projectRoot / "BSP/entities.json";
            if (IsFile(entitiesJson))
            {
                const auto data = mod::Json(entitiesJson);
                for (const auto& entity : data["entities"])
                {
                    const auto label = entity.value("script_label", Json());
                    if (label.is_string() && (label == "line_emitter" || label == "looper" || label == "random") && entity.contains("script_sound") && entity["script_sound"].is_string())
                    {
                        auto low = Lower(entity["script_sound"].get<std::string>());
                        ambience.insert(StartsWith(low, "waw/") ? low.substr(4) : low);
                    }
                }
            }
            const auto music = MusicTableAliases(amb);
            remaster = Json{{"profiles", StockProfiles(*stock, std::vector<std::string>(stingerSet.begin(), stingerSet.end()), twinNames)},
                            {"classes", soundClasses},
                            {"twins", twins},
                            {"reference", reference},
                            {"music", std::vector<std::string>(music.begin(), music.end())},
                            {"stingers", MusicStingers(amb, stingerStates)},
                            {"ambience", std::vector<std::string>(ambience.begin(), ambience.end())},
                            {"announcer_events", IsFile(eventsFile) ? mod::Json(eventsFile) : Json::object()},
                            {"announcer_prefix", audio["announcer_alias_prefix"]}};
        }
        // the bank (sounds.write_t6_bank)
        Json bankReport;
        {
            const sound::BankRequest bankRequest{projectRoot / "content_source", bank, projectRoot / "soundbank", budget, reverbSend, remaster,
                                                 templateCsv ? TemplateAliasNames(*templateCsv) : std::vector<std::string>{}, request.flacEncoder, std::nullopt};
            const auto result = [&]
            {
                const timeline::Span span("write the sound bank");
                const profile::Scope scope("sound mix: write the bank");
                return sound::WriteT6Bank(bankRequest);
            }();
            bankReport = result.report;
            budget = result.budget;
        }
        if (bankReport.contains("engine_names") && !bankReport["engine_names"].is_null() && !bankReport["engine_names"].empty())
        {
            const auto& engine = bankReport["engine_names"];
            content["engine_sound_names"] = Json{{"copies", engine["copies"].size()}, {"unmatched", engine["unmatched"]}};
            warnings.push_back("SOUND_ENGINE_NAMES " + std::to_string(engine["copies"].size()) + " BO2 engine sound names play WaW's sound; " +
                               std::to_string(engine["unmatched"].size()) + " template names have no WaW sound and are silent (BO2's bank is not shipped). List: content_source/sounds.bank.json");
        }
        if (bankReport.contains("remaster") && !bankReport["remaster"].is_null() && !bankReport["remaster"].empty())
        {
            content["audio_remaster"] = bankReport["remaster"];
            const auto& rm = bankReport["remaster"];
            std::vector<std::string> roles, groups;
            for (const auto& [r, _] : rm["roles"].items())
                roles.push_back(r);
            std::sort(roles.begin(), roles.end());
            for (const auto& r : roles)
            {
                const auto& info = rm["roles"][r];
                warnings.push_back("AUDIO_REMASTER " + r + ": " + std::to_string(info["aliases"].get<long long>()) + " aliases take the stock BO2 " + r + " mix (" +
                                   info["stock_mix"]["Bus"].get<std::string>() + "/" + info["stock_mix"]["VolumeGroup"].get<std::string>() + ", " +
                                   info["stock_mix"]["PanType"].get<std::string>() + "), volume " + Plus(info["volume_shift"].get<double>()) + " dB");
            }
            for (const auto& [k, _] : rm["groups"].items())
                groups.push_back(k);
            std::sort(groups.begin(), groups.end());
            for (const auto& key : groups)
            {
                const auto& info = rm["groups"][key];
                if (!info.contains("routing"))
                    continue;
                std::string carry;
                if (info.contains("falloff") && !info["falloff"].is_null() && !info["falloff"].empty())
                {
                    const auto& f = info["falloff"];
                    carry = ", " + std::to_string(f["rows"].get<long long>()) + " placed on BO2's " + f["curve"].get<std::string>() + " falloff (median reach " +
                            G("%g", f["median_dist_max"][0].get<double>()) + " -> " + G("%g", f["median_dist_max"][1].get<double>()) + " units)";
                }
                warnings.push_back("AUDIO_MIX " + key + ": " + std::to_string(info["aliases"].get<long long>()) + " aliases mixed as stock BO2's class (" +
                                   info["routing"]["Bus"].get<std::string>() + "/" + info["routing"]["DuckGroup"].get<std::string>() + "), volume " +
                                   Plus(info["volume_shift"].get<double>()) + " dB" + carry);
            }
            if (rm.contains("twins") && !rm["twins"].empty())
            {
                const auto& placed = rm["twins"];
                std::string notIn = placed["not_in_stock"].empty() ? std::string("none") : mod::ListRepr(placed["not_in_stock"].get<std::vector<std::string>>());
                warnings.push_back("AUDIO_MIX " + std::to_string(placed["placed"].size()) + " aliases take the placement of the stock BO2 alias they play for; not in the stock tables: " + notIn);
            }
        }
        content["reverb_send"] = reverbSend;
        SoundMixResult result{{}, std::move(bankReport), std::move(budget)};
        result.report.warnings = warnings.get<std::vector<std::string>>();
        for (auto& [section, value] : content.items())
            result.report.Add(section, std::move(value));
        return result;
    }
} // namespace w2b::pipeline
