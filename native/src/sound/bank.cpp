// The BO2 alias CSV of the converted sounds (see bank.h).
#include "bank.h"

#include "core/files.h"
#include "core/parallel.h"
#include "core/pyre.h"
#include "core/sum.h"
#include "sound/bank_csv.h"
#include "sound/streamed_bank.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace w2b::sound
{
    namespace fs = std::filesystem;
    using py::Json;

    namespace
    {
        const std::vector<std::string> PLAYBACK_NOT_TRANSLATED{
            "driver routing/ducking (stock BO2 groups; original bus gain baked into alias)", "speaker map (stock pan)",
            "alias-triggered ducks (BO2's Duck column: stock gunfire and impacts duck the mix)",
            "voice limits where WaW authored none (kept unlimited; BO2's 58 voices steal by priority)",
            "WaW room reverb presets (I3DL2; stock BO2 radverb of the same room class used)",
            "reverb send level (offset measured from stock BO2 and WaW sends)", "softest voice limit (approximated with priority)", "chain alias",
            "team/cylinder/move/slave/master fields", "falloff curve shape where no stock BO2 curve matches (nearest loudness used)"};
        constexpr double REVERB_SEND_DB = -24.0, CURVE_TOLERANCE = 1e-5;
        constexpr int REFERENCE_MIN_SAMPLES = 20;
        const std::map<int, std::string> T6_LIMIT_TYPES{{0, "none"}, {1, "oldest"}, {2, "reject"}, {3, "priority"}, {4, "priority"}};
        const std::map<int, std::string> T6_STORAGE{{1, "loaded"}, {2, "streamed"}, {3, "primed"}};
        const std::map<std::string, std::pair<std::string, std::string>> GROUP_ROUTING{{"grp_music", {"bus_music", "snp_music"}},
                                                                                       {"grp_voice", {"bus_voice", "snp_voice"}},
                                                                                       {"grp_menu", {"bus_ui", "snp_foley"}},
                                                                                       {"grp_ambience", {"bus_fx", "snp_ambience"}},
                                                                                       {"grp_weapon", {"bus_fx", "snp_wpn_3p"}}};
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

        struct Error : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        std::string Lower(std::string s)
        {
            for (auto& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        std::string Unprefixed(const std::string& name)
        {
            auto low = Lower(name);
            return low.rfind("waw/", 0) == 0 ? low.substr(4) : low;
        }

        Json ReadJson(const fs::path& p)
        {
            return Json::parse(files::ReadText(p));
        }

        std::string Format(const char* format, const double v)
        {
            char text[64];
            std::snprintf(text, sizeof text, format, v);
            return text;
        }

        // f"{value:.6g}"
        std::string Num(const double v)
        {
            return Format("%.6g", v);
        }

        double RoundTo(const double v, const int digits)
        {
            char text[64];
            std::snprintf(text, sizeof text, "%.*f", digits, v);
            return std::strtod(text, nullptr);
        }

        // _dbspl: inverse of T6 Common::DbsplToLinear, with its uint16 silence
        std::string Dbspl(const double gain)
        {
            if (!std::isfinite(gain) || gain < 0 || gain > 1)
                throw Error("invalid linear sound gain " + py::FloatRepr(gain));
            return gain ? Num(std::max(0.0, 100 + 20 * std::log10(gain))) : "0";
        }

        double D(const Json& v)
        {
            return v.get<double>();
        }

        std::optional<double> Number(const Json& row, const char* key)
        {
            if (!row.contains(key))
                return std::nullopt;
            const auto& v = row[key];
            if (v.is_number())
                return v.get<double>();
            double out = 0;
            if (v.is_string() && py::ParseFloat(v.get<std::string>(), out))
                return out;
            return std::nullopt;
        }

        // _median, keeping Python's int 0 for an empty list
        Json Median(std::vector<double> values)
        {
            if (values.empty())
                return 0;
            std::sort(values.begin(), values.end());
            const auto middle = values.size() / 2;
            return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
        }

        double MedianValue(std::vector<double> values)
        {
            const auto m = Median(std::move(values));
            return m.get<double>();
        }

        std::vector<std::vector<double>> CurvePoints(const Json& curve)
        {
            const auto& points = curve["points"];
            const size_t count = curve.contains("pointCount") ? curve["pointCount"].get<size_t>() : points.size();
            std::vector<std::vector<double>> out;
            for (size_t i = 0; i < std::min(count, points.size()); i++)
                out.push_back({points[i][0].get<double>(), points[i][1].get<double>()});
            bool bad = out.size() < 2 || out.front()[0] != 0 || out.back()[0] != 1;
            for (size_t i = 1; i < out.size() && !bad; i++)
                bad = !std::isfinite(out[i][0]) || !std::isfinite(out[i][1]) || out[i][0] < out[i - 1][0];
            if (bad)
                throw Error("invalid curve " + (curve.contains("name") ? curve["name"].dump() : std::string("None")));
            return out;
        }

        double CurveArea(const std::vector<std::vector<double>>& p)
        {
            // sum() of floats: Python compensates (Neumaier)
            std::vector<double> terms;
            for (size_t i = 0; i + 1 < p.size(); i++)
                terms.push_back((p[i + 1][0] - p[i][0]) * (p[i][1] + p[i + 1][1]) / 2);
            return py::Sum(terms);
        }

        bool IsFalloff(const std::vector<std::vector<double>>& p)
        {
            if (std::abs(p[0][1] - 1) > CURVE_TOLERANCE)
                return false;
            for (size_t i = 0; i + 1 < p.size(); i++)
                if (p[i + 1][1] > p[i][1] + CURVE_TOLERANCE)
                    return false;
            return true;
        }

        bool IsAllOn(const std::vector<std::vector<double>>& p)
        {
            for (size_t i = 0; i + 1 < p.size(); i++)
                if (std::abs(p[i][1] - 1) > CURVE_TOLERANCE)
                    return false;
            return true;
        }

        Json FalloffDescriptor(const double distMin, const double distMax, const std::vector<std::vector<double>>& minPoints,
                               const std::vector<std::vector<double>>& maxPoints)
        {
            return Json{{"standard", IsAllOn(minPoints) && IsFalloff(maxPoints)}, {"falloff", std::max(distMax - distMin, 0.0) * CurveArea(maxPoints)}};
        }

        // _category: stock BO2 bus/group/duck group for the WaW bus by its role
        std::tuple<std::string, std::string, std::string, std::string> Category(const std::string& busName, const Json& busGroups)
        {
            const auto bus = Lower(busName);
            if (busGroups.is_object() && busGroups.contains(bus) && busGroups[bus].is_string() && !busGroups[bus].get<std::string>().empty())
            {
                const auto group = busGroups[bus].get<std::string>();
                const auto it = GROUP_ROUTING.find(group);
                auto [target, duck] = it == GROUP_ROUTING.end() ? std::pair<std::string, std::string>{"bus_fx", "snp_foley"} : it->second;
                if (group == "grp_weapon" && bus.find("1st") != std::string::npos)
                    duck = "snp_wpn_1p";
                return {target, group, duck, group == "grp_music" ? "yes" : "no"};
            }
            const auto has = [&](std::initializer_list<const char*> keys) { return std::any_of(keys.begin(), keys.end(), [&](const char* k) { return bus.find(k) != std::string::npos; }); };
            if (bus.find("music") != std::string::npos || bus.substr(0, 3) == "mus")
                return {"bus_music", "grp_music", "snp_music", "yes"};
            if (has({"voice", "vox", "dialog"}))
                return {"bus_voice", "grp_voice", "snp_voice", "no"};
            if (has({"ui", "menu", "hud"}))
                return {"bus_ui", "grp_menu", "snp_foley", "no"};
            if (has({"amb", "element"}))
                return {"bus_fx", "grp_ambience", "snp_ambience", "no"};
            if (has({"wpn", "weap", "rfl", "pis", "smg", "mg_", "shot", "explo"}))
                return {"bus_fx", "grp_weapon", bus.find("1st") != std::string::npos ? "snp_wpn_1p" : "snp_wpn_3p", "no"};
            return {"bus_fx", "grp_foley", "snp_foley", "no"};
        }

        double SendOffset(const Json& reverbSend, const std::string& group)
        {
            if (reverbSend.is_null() || reverbSend.empty())
                return REVERB_SEND_DB;
            if (reverbSend.contains("roles") && reverbSend["roles"].contains(group))
                return D(reverbSend["roles"][group]["offset_db"]);
            return D(reverbSend["db"]);
        }

        // decode_flags (measured in the official T4 linker)
        Json DecodeFlags(const Json& value)
        {
            if (!value.is_number_integer())
                throw Error("sound flags must be an unsigned 32-bit value");
            const auto flags = value.get<long long>();
            if (flags < 0 || flags > 0xffffffffLL)
                throw Error("sound flags must be an unsigned 32-bit value");
            Json out = Json::object();
            for (const auto& [name, bit] : std::vector<std::pair<const char*, int>>{{"looping", 0}, {"master", 1}, {"slave", 2}, {"fullDryLevel", 3}, {"noWetLevel", 4},
                                                                                    {"randomLooping", 5}, {"spatialized", 6}, {"realDelay", 7}, {"distanceLpf", 8},
                                                                                    {"doppler", 11}, {"isBig", 12}})
                out[name] = static_cast<bool>(flags & (1LL << bit));
            out["loadType"] = (flags >> 13) & 3;
            return out;
        }

        // t6_alias_rows: rows of the official BO2 alias CSV for one bound alias list
        std::pair<std::vector<Json>, Json> AliasRows(const Json& document, const std::set<std::string>& weaponAliases, const Json& reverbSend, const Json& classes)
        {
            std::vector<Json> rows;
            Json errors = Json::array();
            const auto docName = document["name"].get<std::string>();
            const auto busGroups = classes.is_object() ? classes.value("bus_groups", Json()) : Json();
            const auto weaponGroups = classes.is_object() ? classes.value("weapon_dependency_group", Json()) : Json();
            size_t index = 0;
            for (const auto& row : document["aliases"])
            {
                const auto variant = index++;
                const auto& source = row["source"];
                const auto binding = row.value("pcm_binding", Json());
                const auto curves = row.value("t6_curves", Json::object());
                if (binding.is_null() || binding.empty())
                {
                    errors.push_back(Json{{"alias", docName}, {"variant", variant}, {"reason", "no PCM binding"}});
                    continue;
                }
                if (curves.size() != 4)
                {
                    errors.push_back(Json{{"alias", docName}, {"variant", variant}, {"reason", "falloff curves not bound"}});
                    continue;
                }
                const auto& flags = row["flags"];
                const int limits[2] = {source.value("limitType", 0), source.value("entityLimitType", 0)};
                if (!T6_LIMIT_TYPES.count(limits[0]) || !T6_LIMIT_TYPES.count(limits[1]))
                {
                    errors.push_back(Json{{"alias", docName}, {"variant", variant}, {"reason", "unknown T4 voice limit type"}});
                    continue;
                }
                auto [bus, group, duckGroup, music] = Category(row["bus"]["name"].get<std::string>(), busGroups);
                const auto shortName = docName.rfind("waw/", 0) == 0 ? docName.substr(4) : docName;
                const bool spatialized = flags["spatialized"].get<bool>();
                if (weaponAliases.count(shortName) && (weaponGroups.is_null() ? bus == "bus_fx" : weaponGroups.contains(group)))
                {
                    group = weaponGroups.is_null() ? std::string("grp_weapon") : weaponGroups[group].get<std::string>();
                    duckGroup = spatialized ? "snp_wpn_3p" : "snp_wpn_1p";
                }
                const double gain = row["bus"].contains("volumeMod") ? D(row["bus"]["volumeMod"]) : 1.0;
                const auto secondary = row["references"].value("secondaryAliasName", std::string());
                Json waw;
                try
                {
                    waw = FalloffDescriptor(D(source["distMin"]), D(source["distMax"]), CurvePoints(row["curves"]["volumeMinFalloffCurve"]),
                                            CurvePoints(row["curves"]["volumeFalloffCurve"]));
                }
                catch (const std::exception&)
                {
                    waw = Json{{"standard", false}, {"falloff", 0.0}};
                }
                waw["wet_ratio"] = D(source["distMax"]) > 0 ? D(source["distReverbMax"]) / D(source["distMax"]) : 1.0;
                const double offset = SendOffset(reverbSend, group);
                Json out{{"_waw", waw}, {"Name", docName}, {"FileSource", binding["output"]}, {"Secondary", secondary},
                         {"Storage", T6_STORAGE.count(flags["loadType"].get<int>()) ? T6_STORAGE.at(flags["loadType"].get<int>()) : std::string("loaded")},
                         {"Bus", bus}, {"VolumeGroup", group}, {"DuckGroup", duckGroup}, {"Duck", ""},
                         {"ReverbSend", Dbspl(flags["noWetLevel"].get<bool>() ? 0.0 : std::min(D(source["reverbSend"]) * std::pow(10.0, offset / 20), 1.0))},
                         {"CenterSend", Dbspl(D(source["centerPercentage"]))},
                         {"VolMin", Dbspl(D(source["volMin"]) * gain)},
                         {"VolMax", Dbspl(D(source["volMax"]) * gain)},
                         {"DistMin", Num(D(source["distMin"]))},
                         {"DistMaxDry", Num(D(source["distMax"]))},
                         {"DistMaxWet", Num(D(source["distReverbMax"]))}};
                for (const auto& [k, v] : curves.items())
                    out[k] = v["t6"];
                const auto str = [](const Json& v) { return v.is_number_integer() ? std::to_string(v.get<long long>()) : (v.is_string() ? v.get<std::string>() : py::FloatRepr(v.get<double>())); };
                out["LimitCount"] = str(source["limitCount"]);
                out["EntityLimitCount"] = str(source["entityLimitCount"]);
                out["LimitType"] = T6_LIMIT_TYPES.at(limits[0]);
                out["EntityLimitType"] = T6_LIMIT_TYPES.at(limits[1]);
                out["PitchMin"] = Num(D(binding["PitchMinCents"]));
                out["PitchMax"] = Num(D(binding["PitchMaxCents"]));
                out["PriorityMin"] = Num(D(source["minPriority"]));
                out["PriorityMax"] = Num(D(source["maxPriority"]));
                out["PriorityThresholdMin"] = Num(D(source["minPriorityThreshold"]));
                out["PriorityThresholdMax"] = Num(D(source["maxPriorityThreshold"]));
                out["PanType"] = spatialized ? "3d" : "2d";
                out["Pan"] = spatialized ? "default" : "front";
                out["Looping"] = flags["looping"].get<bool>() ? "looping" : "nonlooping";
                out["RandomizeType"] = "";
                out["Probability"] = Num(D(source["probability"]));
                out["StartDelay"] = str(source["startDelay"]);
                out["EnvelopMin"] = Num(D(source["envelopMin"]));
                out["EnvelopMax"] = Num(D(source["envelopMax"]));
                out["EnvelopPercent"] = Dbspl(D(source["envelopPercentage"]));
                out["OcclusionLevel"] = Num(D(source["occlusionLevel"]));
                out["IsBig"] = flags["isBig"].get<bool>() ? "yes" : "no";
                out["DistanceLpf"] = flags["distanceLpf"].get<bool>() ? "yes" : "no";
                out["FluxType"] = "none";
                out["FluxTime"] = "0";
                out["Subtitle"] = source.value("subtitle", std::string());
                out["Doppler"] = flags["doppler"].get<bool>() ? "yes" : "no";
                for (const auto& [k, v] : std::vector<std::pair<const char*, std::string>>{
                         {"ContextType", ""}, {"ContextValue", ""}, {"Timescale", "no"}, {"IsMusic", music}, {"IsCinematic", "no"}, {"FadeIn", "0"}, {"FadeOut", "0"},
                         {"Pauseable", "yes"}, {"StopOnEntDeath", "no"}, {"StopOnPlay", ""}, {"DopplerScale", "0"}, {"FutzPatch", ""}, {"VoiceLimit", "no"},
                         {"IgnoreMaxDist", "no"}, {"NeverPlayTwice", "no"}})
                    out[k] = v;
                rows.push_back(out);
            }
            return {rows, errors};
        }

        // clamp_t6_ranges: rows fitted to the T6 column formats
        Json ClampRanges(std::vector<Json>& rows)
        {
            static const std::map<std::string, std::pair<double, double>> ranges{{"DistMin", {0, 65535}}, {"DistMaxDry", {0, 65535}}, {"DistMaxWet", {0, 65535}},
                                                                                 {"LimitCount", {0, 128}}, {"EntityLimitCount", {0, 128}}};
            static const std::vector<std::string> intColumns{"DistMin", "DistMaxDry", "DistMaxWet", "LimitCount", "EntityLimitCount", "PitchMin", "PitchMax",
                                                             "PriorityMin", "PriorityMax", "StartDelay", "EnvelopMin", "EnvelopMax", "FluxTime", "FadeIn",
                                                             "FadeOut", "DopplerScale"};
            Json changed = Json::array();
            for (auto& row : rows)
                for (const auto& column : intColumns)
                {
                    double value = 0;
                    if (!py::ParseFloat(row[column].get<std::string>(), value))
                        throw Error("could not convert string to float: " + row[column].dump());
                    const auto it = ranges.find(column);
                    const double low = it == ranges.end() ? -INFINITY : it->second.first, high = it == ranges.end() ? INFINITY : it->second.second;
                    const auto t6 = static_cast<long long>(std::nearbyint(std::min(std::max(value, low), high)));
                    row[column] = std::to_string(t6);
                    if (static_cast<double>(t6) != value)
                    {
                        Json entry{{"alias", row["Name"]}, {"column", column}, {"source", value}, {"t6", t6}, {"why", !(low <= value && value <= high) ? "range" : "integer column"}};
                        changed.push_back(entry);
                    }
                }
            return changed;
        }

        // m.expand for a full match of one engine family pattern
        std::optional<std::string> FamilySource(const std::string& low, const std::map<std::string, std::vector<size_t>>& byName)
        {
            for (const auto& [pattern, targets] : ENGINE_SOUND_FAMILIES)
            {
                const pyre::Regex full("\\A(?:" + pattern + ")\\Z");
                if (!full.Search(low))
                    continue;
                for (const auto& t : targets)
                {
                    const auto expanded = full.Sub(t, low, 1);
                    if (byName.count(expanded))
                        return expanded;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::map<std::string, std::vector<size_t>> ByName(const std::vector<Json>& rows)
        {
            std::map<std::string, std::vector<size_t>> out;
            for (size_t i = 0; i < rows.size(); i++)
                out[Unprefixed(rows[i]["Name"].get<std::string>())].push_back(i);
            return out;
        }

        Json EngineNameCopies(std::vector<Json>& rows, const std::vector<std::string>& names, const Json& twins)
        {
            const auto byName = ByName(rows);
            Json copies = Json::object(), unmatched = Json::array();
            std::vector<Json> added;
            long long placed = 0;
            for (const auto& name : names)
            {
                const auto low = Lower(name);
                const auto source = FamilySource(low, byName);
                if (!source)
                {
                    unmatched.push_back(name);
                    continue;
                }
                copies[name] = *source;
                const auto twin = twins.is_object() && twins.contains(low) ? twins[low] : Json::object();
                placed += !twin.empty();
                for (const auto i : byName.at(*source))
                {
                    auto copy = rows[i];
                    copy["Name"] = name;
                    for (const auto& [k, v] : twin.items())
                        copy[k] = v;
                    added.push_back(copy);
                }
            }
            rows.insert(rows.end(), added.begin(), added.end());
            return Json{{"copies", copies}, {"unmatched", unmatched}, {"stock_mix", placed}};
        }

        std::set<std::string> Chained(const std::vector<Json>& rows, const std::map<std::string, std::vector<size_t>>& byName, const std::vector<std::string>& names)
        {
            std::set<std::string> found;
            std::vector<std::string> pending;
            for (const auto& n : names)
                pending.push_back(Unprefixed(n));
            while (!pending.empty())
            {
                const auto name = pending.back();
                pending.pop_back();
                if (!found.insert(name).second)
                    continue;
                if (const auto it = byName.find(name); it != byName.end())
                    for (const auto i : it->second)
                        if (rows[i].contains("Secondary") && !rows[i]["Secondary"].get<std::string>().empty())
                            pending.push_back(Unprefixed(rows[i]["Secondary"].get<std::string>()));
            }
            return found;
        }

        // fit_loaded_budget: weapon chains, then played sounds, then the smallest
        Json FitLoadedBudget(std::vector<Json>& rows, const fs::path& pcmRoot, Json& budget, const std::set<std::string>& weaponAliases)
        {
            std::map<std::string, long long> sizes;
            for (const auto& row : rows)
                if (row["Storage"] == "loaded")
                {
                    const auto file = row["FileSource"].get<std::string>();
                    if (!sizes.count(file))
                        sizes[file] = static_cast<long long>(fs::file_size(pcmRoot / files::FromUtf8(file)));
                }
            const auto byName = ByName(rows);
            const auto weaponChain = Chained(rows, byName, std::vector<std::string>(weaponAliases.begin(), weaponAliases.end()));
            std::set<std::string> weaponFiles, playedFiles;
            for (const auto& n : weaponChain)
                if (const auto it = byName.find(n); it != byName.end())
                    for (const auto i : it->second)
                        weaponFiles.insert(rows[i]["FileSource"].get<std::string>());
            std::vector<std::string> playedNames;
            if (budget.contains("played"))
            {
                playedNames = budget["played"].get<std::vector<std::string>>();
                budget.erase("played");
            }
            for (const auto& n : Chained(rows, byName, playedNames))
                if (const auto it = byName.find(n); it != byName.end())
                    for (const auto i : it->second)
                        playedFiles.insert(rows[i]["FileSource"].get<std::string>());
            std::vector<std::pair<std::string, long long>> order(sizes.begin(), sizes.end());
            std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b)
                      {
                          return std::tuple(!weaponFiles.count(a.first), !playedFiles.count(a.first), a.second, a.first) <
                                 std::tuple(!weaponFiles.count(b.first), !playedFiles.count(b.first), b.second, b.first);
                      });
            std::set<std::string> keep;
            long long used = 0;
            const long long entries = budget["entries"].get<long long>(), bytes = budget["bytes"].get<long long>();
            for (const auto& [source, size] : order)
            {
                if (static_cast<long long>(keep.size()) >= entries || used + size > bytes)
                    continue;
                keep.insert(source);
                used += size;
            }
            Json streamed = Json::array();
            for (auto& row : rows)
                if (row["Storage"] == "loaded" && !keep.count(row["FileSource"].get<std::string>()))
                {
                    row["Storage"] = "streamed";
                    streamed.push_back(Json{{"alias", row["Name"]}, {"file", row["FileSource"]}, {"bytes", sizes[row["FileSource"].get<std::string>()]}});
                }
            if (budget.contains("entry_pool"))
            {
                std::set<std::string> files;
                for (const auto& r : rows)
                    if (r.contains("FileSource") && !r["FileSource"].get<std::string>().empty())
                        files.insert(r["FileSource"].get<std::string>());
                budget["entry_pool"]["map"] = files.size();
            }
            const auto countIn = [&](const std::set<std::string>& a, const auto& pred)
            {
                long long n = 0;
                for (const auto& f : a)
                    n += pred(f);
                return n;
            };
            budget["loaded_files"] = keep.size();
            budget["loaded_bytes"] = used;
            budget["streamed_files"] = static_cast<long long>(sizes.size()) - static_cast<long long>(keep.size());
            budget["weapon_files_loaded"] = countIn(weaponFiles, [&](const std::string& f) { return keep.count(f) > 0; });
            budget["weapon_files_streamed"] = countIn(weaponFiles, [&](const std::string& f) { return sizes.count(f) && !keep.count(f); });
            budget["played_files_loaded"] = countIn(playedFiles, [&](const std::string& f) { return keep.count(f) > 0; });
            budget["played_files_streamed"] = countIn(playedFiles, [&](const std::string& f) { return sizes.count(f) && !keep.count(f); });
            return streamed;
        }

        // audioremaster
        std::optional<std::string> Role(const std::optional<std::string>& bus, const bool spatialized, const std::string& name, const std::set<std::string>& music,
                                        const std::set<std::string>& ambience, const Json& stingers)
        {
            const auto shortName = Unprefixed(name);
            if (stingers.is_object() && stingers.contains(shortName))
                return "stinger:" + stingers[shortName].get<std::string>();
            if (bus == "music" || music.count(shortName))
                return "music";
            if (bus == "ambience" || ambience.count(shortName))
                return "ambience";
            if (bus == "voice" && !spatialized)
                return "announcer";
            return std::nullopt;
        }

        Json AnnouncerCopies(std::vector<Json>& rows, std::map<std::string, std::string>& roles, const Json& events, const std::string& prefix)
        {
            static const pyre::Regex unsafe("[^a-z0-9_]");
            std::map<std::string, std::string> wanted;
            for (const auto& [_, aliases] : events.items())
                for (const auto& alias : aliases)
                    wanted["waw/" + alias.get<std::string>()] = "vox_zmba_" + prefix + unsafe.Sub("_", alias.get<std::string>());
            std::vector<Json> copies;
            Json made = Json::object();
            for (const auto& row : rows)
            {
                const auto name = Lower(row["Name"].get<std::string>());
                const auto it = wanted.find(name);
                if (it != wanted.end() && roles.count(name) && roles[name] == "announcer")
                {
                    auto copy = row;
                    copy["Name"] = it->second;
                    copies.push_back(copy);
                    made[name] = it->second;
                    roles[it->second] = "announcer";
                }
            }
            rows.insert(rows.end(), copies.begin(), copies.end());
            return made;
        }

        void Shift(const std::vector<Json*>& rows, const double shift)
        {
            for (auto* row : rows)
                for (const char* column : {"VolMin", "VolMax"})
                    if (const auto value = Number(*row, column))
                        (*row)[column] = Format("%g", std::max(0.0, std::min(100.0, *value + shift)));
        }

        std::vector<double> Percentiles(std::vector<double> values)
        {
            std::sort(values.begin(), values.end());
            std::vector<double> out;
            for (int i = 0; i <= 100; i++)
                out.push_back(values[static_cast<size_t>(std::nearbyint(i * (static_cast<double>(values.size()) - 1) / 100))]);
            return out;
        }

        double Transfer(const double value, const std::vector<double>& source, const std::vector<double>& target)
        {
            const auto low = static_cast<long long>(std::lower_bound(source.begin(), source.end(), value) - source.begin());
            const auto high = static_cast<long long>(std::upper_bound(source.begin(), source.end(), value) - source.begin()) - 1;
            double rank;
            if (high >= low)
                rank = (low + high) / 2.0;
            else if (low == 0)
                rank = 0.0;
            else if (low >= static_cast<long long>(source.size()))
                rank = static_cast<double>(source.size()) - 1.0;
            else
                rank = low - 1 + (value - source[low - 1]) / (source[low] - source[low - 1]);
            const auto index = std::min(static_cast<long long>(rank), static_cast<long long>(target.size()) - 2);
            return target[index] + (target[index + 1] - target[index]) * (rank - index);
        }

        bool Places(const Json& row)
        {
            return row.value("PanType", std::string()) == "3d" && row.contains("_waw") && row["_waw"].value("standard", false);
        }

        std::vector<double> Doubles(const Json& list)
        {
            std::vector<double> out;
            for (const auto& v : list)
                out.push_back(v.get<double>());
            return out;
        }

        std::pair<std::optional<std::vector<double>>, std::string> SourceQuantiles(const std::string& key, const std::string& column, const std::vector<Json*>& members,
                                                                                   const Json& reference)
        {
            if (reference.is_object() && reference.contains(key) && reference[key].contains(column))
                return {Doubles(reference[key][column]["q"]), "stock_waw"};
            std::vector<double> values;
            for (const auto* row : members)
            {
                if (column == "Falloff")
                {
                    if (Places(*row))
                        values.push_back(D((*row)["_waw"]["falloff"]));
                }
                else if (column == "DistMin")
                {
                    if (Places(*row))
                        if (const auto v = Number(*row, column.c_str()))
                            values.push_back(*v);
                }
                else if (const auto v = Number(*row, column.c_str()))
                    values.push_back(*v);
            }
            if (static_cast<int>(values.size()) >= REFERENCE_MIN_SAMPLES)
                return {Percentiles(values), "map"};
            return {std::nullopt, ""};
        }

        std::vector<double> ColumnValues(const std::vector<Json*>& rows, const char* column)
        {
            std::vector<double> out;
            for (const auto* row : rows)
                if (const auto v = Number(*row, column))
                    out.push_back(*v);
            return out;
        }

        Json MixClass(const std::string& key, const std::vector<Json*>& members, const Json& stock, const Json& reference, const std::string& reach = "waw")
        {
            Json out{{"routing", stock["routing"]}, {"transferred", Json::object()}};
            for (auto* row : members)
                for (const auto& [k, v] : stock["routing"].items())
                    (*row)[k] = v;
            const auto& targets = stock["quantiles"];
            const auto falloff = stock.value("falloff", Json());
            std::vector<Json*> ordered;
            for (auto* row : members)
                if (Number(*row, "PriorityMin").value_or(0.0) <= Number(*row, "PriorityMax").value_or(0.0))
                    ordered.push_back(row);
            for (const char* column : {"PriorityMin", "PriorityMax", "OcclusionLevel"})
            {
                const auto [source, origin] = SourceQuantiles(key, column, members, reference);
                if (!source || !targets.contains(column))
                    continue;
                auto beforeValues = ColumnValues(members, column);
                const auto before = beforeValues.empty() ? Json(0) : Median(beforeValues);
                const auto target = Doubles(targets[column]);
                for (auto* row : members)
                    if (const auto value = Number(*row, column))
                    {
                        const double moved = Transfer(*value, *source, target);
                        (*row)[column] = std::string(column) == "OcclusionLevel" ? Format("%.3g", moved) : std::to_string(static_cast<long long>(std::nearbyint(moved)));
                    }
                auto afterValues = ColumnValues(members, column);
                const auto after = afterValues.empty() ? Json(0) : Median(afterValues);
                out["transferred"][column] = Json{{"reference", origin}, {"median", Json::array({before, after})}};
            }
            for (auto* row : ordered)
                if (Number(*row, "PriorityMin").value_or(0.0) > Number(*row, "PriorityMax").value_or(0.0))
                    (*row)["PriorityMin"] = (*row)["PriorityMax"];
            const auto [nearQ, nearOrigin] = SourceQuantiles(key, "DistMin", members, reference);
            const auto [farQ, farOrigin] = SourceQuantiles(key, "Falloff", members, reference);
            std::vector<Json*> placed;
            for (auto* row : members)
                if (Places(*row))
                    placed.push_back(row);
            if (reach == "stock" && !falloff.is_null() && !falloff.empty() && !placed.empty() && nearQ && farQ && targets.contains("DistMin") && targets.contains("Falloff"))
            {
                std::vector<double> beforeValues;
                for (auto* row : placed)
                    beforeValues.push_back(Number(*row, "DistMaxDry").value_or(0.0));
                const auto before = MedianValue(beforeValues);
                for (auto* row : placed)
                {
                    const double start = Transfer(Number(*row, "DistMin").value_or(0.0), *nearQ, Doubles(targets["DistMin"]));
                    const double end = start + Transfer(D((*row)["_waw"]["falloff"]), *farQ, Doubles(targets["Falloff"])) / D(falloff["area"]);
                    (*row)["DistMin"] = std::to_string(static_cast<long long>(std::nearbyint(start)));
                    (*row)["DistMaxDry"] = std::to_string(static_cast<long long>(std::nearbyint(end)));
                    (*row)["DistMaxWet"] = std::to_string(static_cast<long long>(std::nearbyint(end * (*row)["_waw"].value("wet_ratio", 1.0))));
                    (*row)["DryMaxCurve"] = falloff["DryMaxCurve"];
                    const auto& curves = falloff["falloff_curves"];
                    if (row->contains("WetMaxCurve") && std::find(curves.begin(), curves.end(), (*row)["WetMaxCurve"]) != curves.end())
                        (*row)["WetMaxCurve"] = falloff["WetMaxCurve"];
                }
                long long spatial = 0;
                for (auto* row : members)
                    spatial += row->value("PanType", std::string()) == "3d";
                std::vector<double> afterValues;
                for (auto* row : placed)
                    afterValues.push_back(Number(*row, "DistMaxDry").value_or(0.0));
                out["falloff"] = Json{{"rows", placed.size()},
                                      {"kept_authored_shape", spatial - static_cast<long long>(placed.size())},
                                      {"curve", falloff["DryMaxCurve"]},
                                      {"reach", reach},
                                      {"reference", Json::array({nearOrigin, farOrigin})},
                                      {"median_dist_max", Json::array({before, MedianValue(afterValues)})}};
            }
            return out;
        }

        Json Remaster(std::vector<Json>& rows, const std::map<std::string, std::string>& roles, const Json& profiles, const Json& reference, const Json& twins)
        {
            Json report{{"roles", Json::object()}, {"unmeasured", Json::array()}, {"groups", Json::object()}};
            std::map<std::string, std::vector<Json*>> byRole, byGroup;
            for (auto& row : rows)
            {
                const auto name = Lower(row["Name"].get<std::string>());
                if (const auto it = roles.find(name); it != roles.end())
                    byRole[it->second].push_back(&row);
                else
                    byGroup[row.value("VolumeGroup", std::string()) + "|" + row.value("PanType", std::string())].push_back(&row);
            }
            const auto groups = profiles.value("groups", Json::object());
            const auto classes = profiles.value("classes", Json::object());
            const auto nameCount = [](const std::vector<Json*>& members)
            {
                std::set<std::string> names;
                for (const auto* row : members)
                    names.insert((*row)["Name"].get<std::string>());
                return names.size();
            };
            for (auto& [key, members] : byGroup)
            {
                const auto wawVolumes = ColumnValues(members, "VolMax");
                if (!groups.contains(key) || groups[key].is_null() || wawVolumes.empty())
                {
                    report["unmeasured"].push_back(key);
                    continue;
                }
                const auto& stock = groups[key];
                const bool level = reference.is_object() && reference.contains(key) && reference[key].contains("VolMax") && !reference[key]["VolMax"].is_null();
                const double shift = D(stock["volume"]) - (level ? D(reference[key]["VolMax"]["q"][50]) : MedianValue(wawVolumes));
                Shift(members, shift);
                report["groups"][key] = Json{{"aliases", nameCount(members)}, {"rows", members.size()}, {"volume_shift", RoundTo(shift, 2)}, {"stock_rows", stock["rows"]},
                                             {"level_reference", level ? "stock_waw" : "map"}};
                if (classes.contains(key))
                    report["groups"][key].update(MixClass(key, members, classes[key], reference));
            }
            for (auto& [r, members] : byRole)
            {
                if (!profiles.contains(r) || profiles[r].is_null())
                {
                    report["unmeasured"].push_back(r);
                    continue;
                }
                const auto& profile = profiles[r];
                const auto wawVolumes = ColumnValues(members, "VolMax");
                const double shift = !profile["volume"].is_null() && !wawVolumes.empty() ? D(profile["volume"]) - MedianValue(wawVolumes) : 0.0;
                Json mix = Json::object();
                for (const auto& [k, v] : profile["mix"].items())
                    if (!(r == "ambience" && k == "PanType"))
                        mix[k] = v;
                long long placed = 0;
                for (auto* row : members)
                    for (const auto& [k, v] : mix.items())
                        (*row)[k] = v;
                Json spread;
                if (r == "ambience" && classes.contains("grp_ambience|3d"))
                {
                    const auto& ambient = classes["grp_ambience|3d"];
                    if (ambient.contains("falloff") && !ambient["falloff"].is_null() && !ambient["falloff"].empty())
                    {
                        std::vector<Json*> spatial;
                        for (auto* row : members)
                            if (row->value("PanType", std::string()) == "3d")
                                spatial.push_back(row);
                        auto copy = ambient;
                        copy["routing"] = Json::object();
                        spread = MixClass("grp_ambience|3d", spatial, copy, reference, "stock").value("falloff", Json());
                    }
                }
                if (r == "ambience" && (spread.is_null() || spread.empty()))
                    for (auto* row : members)
                    {
                        const auto emitters = profile.value("emitters", Json::object());
                        const auto looping = row->value("Looping", std::string());
                        if (emitters.contains(looping) && !emitters[looping].is_null() && row->value("PanType", std::string()) == "3d")
                        {
                            for (const auto& [k, v] : emitters[looping]["columns"].items())
                                (*row)[k] = v;
                            placed++;
                        }
                    }
                Shift(members, shift);
                report["roles"][r] = Json{{"aliases", nameCount(members)}, {"rows", members.size()}, {"stock_mix", profile["mix"]}, {"volume_shift", RoundTo(shift, 2)},
                                          {"stock_rows", profile["rows"]}};
                if (placed)
                    report["roles"][r]["stock_emitter_rows"] = placed;
                if (!spread.is_null() && !spread.empty())
                    report["roles"][r]["falloff"] = spread;
            }
            if (twins.is_object() && !twins.empty())
            {
                const auto stock = profiles.value("twins", Json::object());
                Json placedTwins = Json::object();
                std::set<std::string> missing;
                for (auto& row : rows)
                {
                    const auto name = Lower(row["Name"].get<std::string>());
                    if (!twins.contains(name))
                        continue;
                    const auto twin = twins[name].get<std::string>();
                    if (stock.contains(twin))
                    {
                        for (const auto& [k, v] : stock[twin].items())
                            row[k] = v;
                        placedTwins[row["Name"].get<std::string>()] = twin;
                    }
                    else
                        missing.insert(twin);
                }
                report["twins"] = Json{{"placed", placedTwins}, {"not_in_stock", std::vector<std::string>(missing.begin(), missing.end())}};
            }
            return report;
        }

        // animnotes.notification_offset: the note count after a raw v17 xanim's motion tracks
        size_t NotificationOffset(const std::string& raw)
        {
            const auto u16 = [&](const size_t at)
            {
                uint16_t v;
                std::memcpy(&v, raw.data() + at, 2);
                return v;
            };
            const unsigned frames = u16(2), bones = u16(4), flags = static_cast<unsigned char>(raw[6]);
            const unsigned numframes = flags & 1 ? frames : frames - 1;
            const unsigned loopFrames = numframes + 1;
            const bool byteIndices = numframes < 256;
            size_t pos = 10;
            const auto count = [&]
            {
                pos += 2;
                return u16(pos - 2);
            };
            const auto keyed = [&](const unsigned n, const unsigned size)
            {
                if (n < loopFrames)
                    pos += byteIndices ? n : 2 * n;
                pos += static_cast<size_t>(n) * size;
            };
            const auto trans = [&]
            {
                const unsigned n = count();
                if (n == 1)
                    pos += 12;
                else if (n > 1)
                {
                    keyed(n, 0);
                    const bool small = raw[pos] != 0;
                    pos += 1 + 24 + static_cast<size_t>(n) * (small ? 3 : 6);
                }
            };
            if (flags & 2)
            {
                const unsigned n = count();
                pos += n == 1 ? 2 : 0;
                if (n > 1)
                    keyed(n, 2);
                trans();
            }
            if (bones)
            {
                const size_t mask = (bones + 7) / 8;
                const auto half = raw.substr(pos + mask, mask);
                pos += 2 * mask;
                for (unsigned b = 0; b < bones; b++)
                    pos = raw.find('\0', pos) + 1;
                for (unsigned bone = 0; bone < bones; bone++)
                {
                    const unsigned stored = (static_cast<unsigned char>(half[bone / 8]) >> (bone % 8)) & 1 ? 2 : 6;
                    const unsigned n = count();
                    if (n == 1)
                        pos += stored;
                    else if (n > 1)
                        keyed(n, stored);
                    trans();
                }
            }
            return pos;
        }

        // animnotes.actor_sound_aliases: sound notes owned by actors, through their animation trees
        std::set<std::string> ActorSoundAliases(const fs::path& project)
        {
            static const pyre::Regex usingTree("#using_animtree\\s*\\(\\s*\"([^\"]+)\"");
            static const pyre::Regex clip("[\\w/]+");
            std::set<std::string> trees, clips, aliases;
            std::error_code e;
            if (fs::is_directory(project / "aitype", e))
                for (auto it = fs::recursive_directory_iterator(project / "aitype", e); it != fs::recursive_directory_iterator(); ++it)
                {
                    auto ext = Lower(files::Utf8(it->path().extension()));
                    if (ext == ".gsc" || ext == ".csc")
                        for (const auto& m : usingTree.FindAll(files::ReadText(it->path())))
                            trees.insert(m[0]);
                }
            for (const auto& tree : trees)
            {
                const auto path = project / "animtrees" / files::FromUtf8(tree + ".atr");
                if (fs::is_regular_file(path, e))
                    for (const auto& m : clip.FindAll(files::ReadText(path)))
                        clips.insert(m[0]);
            }
            for (const auto& name : clips)
            {
                const auto path = project / "xanim" / files::FromUtf8(name);
                std::string raw;
                if (!fs::is_regular_file(path, e) || !files::ReadBytes(path, raw) || raw.find("sndnt#") == std::string::npos)
                    continue;
                const auto start = NotificationOffset(raw);
                size_t pos = start + 1;
                for (unsigned i = 0; i < static_cast<unsigned char>(raw[start]); i++)
                {
                    const auto end = raw.find('\0', pos);
                    const auto note = raw.substr(pos, end - pos); // latin-1: one byte, one code point
                    if (note.rfind("sndnt#", 0) == 0)
                    {
                        std::string utf8;
                        for (const unsigned char c : note.substr(6))
                        {
                            if (c < 0x80)
                                utf8 += static_cast<char>(c);
                            else
                                utf8 += static_cast<char>(0xC0 | (c >> 6)), utf8 += static_cast<char>(0x80 | (c & 0x3F));
                        }
                        aliases.insert(Lower(utf8));
                    }
                    pos = end + 3;
                }
            }
            return aliases;
        }

        Json DefaultReverbMix(const fs::path& project)
        {
            const auto path = project / "sound_ir/driver_globals.json";
            std::error_code e;
            if (!fs::is_regular_file(path, e))
                return Json{{"status", "missing_source_default"}};
            const auto data = ReadJson(path);
            std::vector<Json> patches;
            for (const auto& p : data.value("reverbPatches", Json::array()))
                if (Lower(p.value("name", std::string())) == "default")
                    patches.push_back(p);
            if (patches.size() != 1)
                return Json{{"status", "missing_source_default"}};
            const auto& patch = patches[0];
            const bool silent = D(patch["room"]) <= -10000 || (D(patch["reflections"]) <= -10000 && D(patch["reverb"]) <= -10000);
            Json out{{"status", silent ? "silent_source_default" : "unsupported_source_default"}, {"source", patch}};
            if (silent)
                out["dry"] = 1, out["wet"] = 0;
            return out;
        }

    } // namespace

    Json WriteT6Bank(const Json& request)
    {
        const auto project = files::FromUtf8(request["project"].get<std::string>());
        const auto bank = request["bank"].get<std::string>();
        const auto destination = files::FromUtf8(request["destination"].get<std::string>());
        auto budget = request.value("budget", Json());
        const auto reverbSendIn = request.value("reverb_send", Json());
        const auto remaster = request.value("remaster", Json());
        const auto engineNames = request.value("engine_names", Json::array()).get<std::vector<std::string>>();
        const auto bindings = ReadJson(project / "sounds.bindings.json");
        std::set<std::string> weaponAliases;
        std::error_code e;
        if (fs::is_regular_file(project / "weapons.stage.json", e))
        {
            const auto stage = ReadJson(project / "weapons.stage.json");
            for (const auto& weapon : stage["weapons"])
                for (const auto& name : weapon.value("dependencies", Json::object()).value("sound", Json::array()))
                    weaponAliases.insert(name.get<std::string>());
        }
        Json report{{"status", "t6_playback_bank_not_full_semantics"},
                    {"bank", bank},
                    {"aliases", 0},
                    {"variants", 0},
                    {"not_translated", PLAYBACK_NOT_TRANSLATED},
                    {"volume_encoding", "linear gain to T6 dB SPL"},
                    {"source_bus_gain_applied", true},
                    {"default_reverb", DefaultReverbMix(project)},
                    {"reverb_send", !reverbSendIn.is_null() && !reverbSendIn.empty() ? reverbSendIn
                                                                                       : Json{{"status", "unmeasured_default"}, {"db", REVERB_SEND_DB}, {"reason", "no stock data supplied"}}},
                    {"voice_limit_approximations", Json::array()},
                    {"errors", Json::array()}};
        std::vector<Json> rows;
        std::map<std::string, std::string> roles;
        const bool remastering = !remaster.is_null();
        std::set<std::string> music, ambience;
        if (remastering)
        {
            for (const auto& m : remaster.value("music", Json::array()))
                music.insert(m.get<std::string>());
            for (const auto& a : remaster.value("ambience", Json::array()))
                ambience.insert(a.get<std::string>());
        }
        const auto& inputs = bindings.at("aliases");
        const auto classes = remastering ? remaster.value("classes", Json()) : Json();
        const auto stingers = remastering ? remaster.value("stingers", Json()) : Json();
        const auto& measuredReverb = report.at("reverb_send");
        struct AliasWork
        {
            std::string name;
            std::optional<std::string> role;
            Json approximations = Json::array();
            Json errors = Json::array();
            std::vector<Json> rows;
        };
        std::vector<AliasWork> converted(inputs.size());
        parallel::For(inputs.size(), [&](std::size_t i)
        {
            const auto& item = inputs.at(i);
            auto& result = converted[i];
            const auto document = ReadJson(project / files::FromUtf8(item["output"].get<std::string>()));
            if (remastering && !document["aliases"].empty())
            {
                const auto& first = document["aliases"][0];
                std::optional<std::string> bus;
                if (first.contains("bus"))
                {
                    if (first["bus"].is_object())
                    {
                        if (first["bus"].contains("name") && first["bus"]["name"].is_string())
                            bus = first["bus"]["name"].get<std::string>();
                    }
                    else if (first["bus"].is_string())
                        bus = first["bus"].get<std::string>();
                }
                const bool spatialized = (first["source"].value("flags", 0LL) & (1LL << 6)) != 0;
                result.name = Lower(document["name"].get<std::string>());
                result.role = Role(bus, spatialized, document["name"].get<std::string>(), music, ambience, stingers);
            }
            auto [fresh, errors] = AliasRows(document, weaponAliases, measuredReverb, classes);
            size_t index = 0;
            for (const auto& row : document["aliases"])
            {
                for (const char* field : {"limitType", "entityLimitType"})
                    if (row["source"].contains(field) && row["source"][field] == 4)
                        result.approximations.push_back(
                            Json{{"alias", document["name"]}, {"variant", index}, {"field", field}, {"source", "softest"}, {"t6", "priority"}});
                index++;
            }
            result.rows = std::move(fresh);
            result.errors = std::move(errors);
        }, 4);
        long long aliases = 0;
        for (auto& result : converted)
        {
            if (result.role)
                roles[result.name] = std::move(*result.role);
            for (auto& approximation : result.approximations)
                report["voice_limit_approximations"].push_back(std::move(approximation));
            for (auto& error : result.errors)
                report["errors"].push_back(std::move(error));
            if (!result.rows.empty())
            {
                rows.insert(rows.end(), std::make_move_iterator(result.rows.begin()), std::make_move_iterator(result.rows.end()));
                aliases++;
            }
        }
        report["aliases"] = aliases;
        if (remastering)
        {
            const auto copies = AnnouncerCopies(rows, roles, remaster.value("announcer_events", Json::object()), remaster.value("announcer_prefix", std::string("waw_")));
            report["remaster"] = Remaster(rows, roles, remaster["profiles"], remaster.value("reference", Json()), remaster.value("twins", Json()));
            report["remaster"]["announcer_copies"] = copies;
            if (remaster.contains("classes") && !remaster["classes"].is_null() && !remaster["classes"].empty())
                report["remaster"]["bus_groups"] = remaster["classes"];
        }
        if (!engineNames.empty())
        {
            report["engine_names"] = EngineNameCopies(rows, engineNames, remastering ? remaster["profiles"].value("twins", Json()) : Json());
            if (!budget.is_null())
            {
                std::set<std::string> played;
                for (const auto& p : budget.value("played", Json::array()))
                    played.insert(p.get<std::string>());
                for (const auto& [_, v] : report["engine_names"]["copies"].items())
                    played.insert(v.get<std::string>());
                budget["played"] = std::vector<std::string>(played.begin(), played.end());
            }
        }
        // animation notes play on the actor's native sound owner: they end on death
        const auto actorAliases = ActorSoundAliases(project.parent_path());
        std::set<std::string> matched;
        for (auto& row : rows)
            if (actorAliases.count(Lower(row["Name"].get<std::string>())))
            {
                row["StopOnEntDeath"] = "yes";
                matched.insert(row["Name"].get<std::string>());
            }
        report["actor_death_cleanup"] = std::vector<std::string>(matched.begin(), matched.end());
        report["clamped"] = ClampRanges(rows);
        if (!budget.is_null())
        {
            std::set<std::string> played;
            for (const auto& p : budget.value("played", Json::array()))
                played.insert(p.get<std::string>());
            for (const auto& a : actorAliases)
                played.insert(Lower(a));
            budget["played"] = std::vector<std::string>(played.begin(), played.end());
            report["loaded_to_streamed"] = FitLoadedBudget(rows, project / "pcm", budget, weaponAliases);
            report["loaded_budget"] = budget;
        }
        if (request.value("stream_flac", StreamFlacEnabled()))
            report["streamed_flac"] = PackStreamedAudio(rows, project / "pcm",
                                                        files::FromUtf8(request.value("flac_encoder", std::string{})));
        std::set<std::string> known;
        for (const auto& r : rows)
            known.insert(r["Name"].get<std::string>());
        for (auto& row : rows)
        {
            const auto secondary = row["Secondary"].get<std::string>();
            if (!secondary.empty() && !known.count(secondary))
            {
                report["errors"].push_back(Json{{"alias", row["Name"]}, {"reason", "secondary alias " + secondary + " not converted"}});
                row["Secondary"] = "";
            }
        }
        report["variants"] = rows.size();
        WriteAliasCsv(destination / files::FromUtf8(bank + ".aliases.csv"), rows);
        files::WriteText(project / "sounds.bank.json", py::JsonDumps(report, 2) + "\n");
        return Json{{"report", report}, {"budget", budget}};
    }

    Json WriteT6BankRequest(const fs::path& path)
    {
        return WriteT6Bank(ReadJson(path));
    }
    Json DefaultReverbMixOf(const fs::path& project)
    {
        return DefaultReverbMix(project);
    }
    size_t NotificationOffsetOf(const std::string& raw)
    {
        return NotificationOffset(raw);
    }
} // namespace w2b::sound
