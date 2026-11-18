// The end of the bridge stage (see bridge_tail.h).
#include "bridge_tail.h"
#include "bridge_sound.h"
#include "bridge_support.h"

#include "core/pyre.h"
#include "coverage_stage.h"
#include "effects/stage.h"
#include "extract/stockwaw.h"
#include "gsc/port/staging.h"
#include "hud_stage.h"
#include "hooks_stage.h"
#include "extract/cache.h"
#include "gsc/port/api.h"
#include "gsc/port/t6api.h"
#include "gsc/port/zone_scripts.h"
#include "weapon_runtime.h"
#include "weapon_visuals.h"
#include "weapons/stage.h"
#include "core/asset_paths.h"
#include "shaders/stage.h"
#include "visions/visions.h"
#include "client_stage.h"
#include "weapons/water_bullets.h"
#include "ingame_menus.h"
#include "lighting_tail.h"
#include "models/identities.h"
#include "models/material_identity.h"
#include "models/surface_fixes.h"
#include "modutil.h"
#include "physics/physics.h"
#include "rumbles.h"
#include "scripts_stage.h"
#include "strings/localization.h"
#include "world/grid_stage.h"
#include "world/primary_lights.h"
#include "world/zone_manifest.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using bridge::AppendZone;
    using bridge::Call;
    using bridge::OptionalPath;
    using bridge::PathList;
    using bridge::PathOrNull;
    using bridge::Paths;
    using bridge::Timed;
    using bridge::Truthy;
    using mod::StartsWith;
    using mod::Str;
    using py::Json;

    namespace
    {
        // stockperks.asset_lines: explicit native perk dependencies
        std::vector<std::string> PerkAssetLines(const fs::path& outRoot)
        {
            static const pyre::Regex call("\\b(precacheitem|precachemodel|precacheshader|loadfx)\\s*\\(\\s*\"([^\"\\n]+)\"", pyre::IGNORECASE);
            const std::map<std::string, std::string> kinds{{"precacheitem", "weapon"}, {"precachemodel", "xmodel"}, {"precacheshader", "material"}, {"loadfx", "fx"}};
            std::vector<fs::path> paths{outRoot / "maps/mp/waw/_waw2bo2_perks.gsc"};
            const auto glob = [&](const fs::path& folder, const std::string& prefix, const std::string& suffix)
            {
                std::vector<fs::path> found;
                std::error_code e;
                if (fs::is_directory(folder, e))
                    for (const auto& entry : fs::directory_iterator(folder, e))
                    {
                        const auto name = mod::Lower(files::Utf8(entry.path().filename()));
                        if (StartsWith(name, prefix) && mod::EndsWith(name, suffix))
                            found.push_back(entry.path());
                    }
                return found;
            };
            for (const auto& p : glob(outRoot / "maps/mp/waw", "_waw2bo2_perk_", ".gsc"))
                paths.push_back(p);
            for (const auto& p : glob(outRoot / "clientscripts/mp/waw", "_waw2bo2_perk_", ".csc"))
                paths.push_back(p);
            std::set<std::string> entries;
            for (const auto& path : paths)
                for (const auto& m : call.FindAll(files::ReadText(path)))
                    entries.insert(kinds.at(mod::Lower(m[0])) + "," + m[1]);
            return {entries.begin(), entries.end()};
        }

        // repr() of a dict of str -> int, as print() shows framework_coverage.totals
        std::string CountsRepr(const Json& counts)
        {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : counts.items())
            {
                out += (first ? "" : ", ") + py::StringRepr(k) + ": " + std::to_string(v.get<long long>());
                first = false;
            }
            return out + "}";
        }
    } // namespace

    Json BridgeTail(const Json& input, textures::IwdRecovery& recovery)
    {
        auto request = input; // the visuals block supplies the later blocks' vision report and HUD materials
        auto report = request.at("report");
        const auto project = request.at("project").get<std::string>();
        const auto projectRoot = files::FromUtf8(request.at("project_root").get<std::string>());
        const auto stage = files::FromUtf8(request.at("stage").get<std::string>());
        const auto roots = PathList(request.at("roots"));
        const auto bo2Root = OptionalPath(request.at("bo2_root"));
        const auto techsetDump = OptionalPath(request.at("techset_dump"));
        const auto wawMapStem = request.at("waw_map_stem").is_string() ? std::optional(request.at("waw_map_stem").get<std::string>()) : std::nullopt;
        auto stockJson = request.value("stock_waw", Json());
        const bool hasScripts = Truthy(report["scripts"]);
        auto& warnings = report["warnings"];
        auto& errors = report["errors"];
        auto& content = report["content"];
        auto images = request.at("images");
        Json weaponRecovered = Json::object();
        if (request.contains("scripts"))
        {
            const auto& scriptsReq = request["scripts"];
            // verify_techsets: every technique set the materials use, with its shaders
            {
                std::set<std::string> used;
                for (const auto& n : scriptsReq.at("used_techsets"))
                    used.insert(n.get<std::string>());
                report["techsets"] = std::vector<std::string>(used.begin(), used.end());
                if (!techsetDump)
                    errors.push_back("no --techset-dump given; the bridge cannot load technique sets without techniquesets/*.json + shader_bin/*.cso dumped from a stock T6 zone");
                else
                {
                    const auto extraRoots = PathList(scriptsReq.at("remaster_roots"));
                    const std::vector<fs::path> lookup{projectRoot, *techsetDump};
                    const auto find = [&](const fs::path& rel) -> std::optional<fs::path>
                    {
                        std::error_code e;
                        for (const auto& root : lookup)
                            if (fs::exists(root / rel, e))
                                return root / rel;
                        return std::nullopt;
                    };
                    const auto shadersOf = [](const Json& techset)
                    {
                        std::set<std::string> found;
                        for (const auto& tech : techset.value("techniques", Json::array()))
                        {
                            if (!Truthy(tech))
                                continue;
                            for (const auto& pass : tech.value("passArray", Json::array()))
                                for (const auto& [key, prefix] : std::vector<std::pair<std::string, std::string>>{{"pixelShader", "ps_"}, {"vertexShader", "vs_"}})
                                    if (pass.contains(key) && pass[key].is_object() && Truthy(pass[key].value("name", Json())))
                                        found.insert("shader_bin/" + prefix + pass[key]["name"].get<std::string>() + ".cso");
                        }
                        return found;
                    };
                    for (const auto& name : used)
                    {
                        const fs::path rel = fs::path("techniquesets") / files::FromUtf8(name + ".json");
                        auto path = find(rel);
                        if (!path)
                        {
                            // outside the donor view: copy it and its shaders from its own zone
                            std::optional<fs::path> owner;
                            for (const auto& r : extraRoots)
                                if (mod::IsFile(r / rel))
                                {
                                    owner = r;
                                    break;
                                }
                            if (!owner)
                                owner = extract::RequiredBo2Techset(*techsetDump, name);
                            if (owner)
                            {
                                std::vector<std::string> rels{mod::Generic(rel)};
                                for (const auto& sh : shadersOf(mod::Json(*owner / rel)))
                                    rels.push_back(sh);
                                for (const auto& r : rels)
                                    if (mod::IsFile(*owner / files::FromUtf8(r)))
                                    {
                                        const auto target = projectRoot / files::FromUtf8(r);
                                        fs::create_directories(target.parent_path());
                                        fs::copy_file(*owner / files::FromUtf8(r), target, fs::copy_options::overwrite_existing);
                                        fs::last_write_time(target, fs::last_write_time(*owner / files::FromUtf8(r)));
                                    }
                            }
                            path = projectRoot / rel;
                        }
                        if (!fs::exists(*path))
                        {
                            errors.push_back("technique set " + name + ": " + Str(*path) + " missing from dump");
                            continue;
                        }
                        for (const auto& sh : shadersOf(mod::Json(*path)))
                            if (!find(files::FromUtf8(sh)))
                                errors.push_back("technique set " + name + ": shader " + sh + " missing from dump");
                    }
                }
            }
            // stage_map_scripts: the template's scripts under the project name, the map's main
            const auto mapScript = OptionalPath(scriptsReq.at("waw_map_script"));
            Json scriptFiles{{"root", Str(projectRoot)}, {"project", project},
                             {"bo2Template", bo2Root ? Json{{"root", Str(*bo2Root / "mods" / "zm_test")}, {"name", "zm_test"}} : Json()},
                             {"requiredTemplate", scriptsReq.at("required_template")}, {"rewriteTemplateReferences", true}};
            if (!scriptsReq.at("drop_zone_manager").is_null())
                scriptFiles["dropZoneManager"] = scriptsReq["drop_zone_manager"];
            const auto staged = Call(gsc::port::StageScriptFilesRequest, scriptFiles);
            for (const auto& e : staged["errors"])
                errors.push_back(e);
            for (const auto& w : staged["warnings"])
                warnings.push_back(w);
            Json mainResult = staged.contains("zoneMain") ? staged["zoneMain"] : Json();
            const auto mainGsc = projectRoot / "maps" / "mp" / files::FromUtf8(project + ".gsc");
            const bool hasMain = fs::exists(mainGsc);
            if (!mapScript || !hasMain)
                errors.push_back("no WaW map script given; the BO2 map script would keep the template's single zone");
            if (hasMain)
            {
                auto graph = scriptsReq.at("zone_graph");
                if (graph.is_null() && mapScript)
                    graph = Call(gsc::port::ZoneScriptsRequest, Json{{"source", Str(*mapScript)}});
                if (mainResult.is_null())
                    mainResult = Call(gsc::port::ZoneScriptsRequest, Json{{"main", Str(mainGsc)}, {"dropManager", mapScript.has_value()}, {"guardSwitch", true}});
                if (Truthy(mainResult.at("error")))
                    errors.push_back("zones: " + mainResult["error"].get<std::string>());
                else if (mapScript)
                    warnings.push_back("zone graph from " + files::Utf8(mapScript->filename()) + ": start zones " +
                                       mod::ListRepr(graph.at("initial").get<std::vector<std::string>>()) + ", " + std::to_string(graph.at("adjacency").size()) + " links");
                if (Truthy(mainResult.at("guardedSwitch")))
                    warnings.push_back("zm_test electric_switch guarded: WaW map has no template switch entities");
            }
            const auto hooked = Call(HookMapScripts, Json{{"project_root", Str(projectRoot)}, {"project", project}, {"map_name", request.at("waw_map_stem")},
                                                          {"bo2_root", PathOrNull(bo2Root)}, {"bo2_stock_perks", request.value("bo2_stock_perks", false)},
                                                          {"scripts", hasScripts}, {"characters", report["scripts"].is_object() && Truthy(report["scripts"].value("characters", Json()))},
                                                          {"staged_animtrees", report["scripts"].is_object() ? report["scripts"].value("staged_animtrees", Json::array()) : Json::array()}});
            for (const auto& w : hooked["warnings"])
                warnings.push_back(w);
            for (const auto& e : hooked["errors"])
                errors.push_back(e);
            for (const auto& [k, v] : hooked["content"].items())
                content[k] = v;
            // stage_rawfiles
            const auto rawfiles = Call(gsc::port::StageScriptFilesRequest, Json{{"root", Str(projectRoot)}, {"project", report["project"]}, {"rawfilesBo2", PathOrNull(bo2Root)}});
            for (const auto& e : rawfiles["errors"])
                errors.push_back(e);
            for (const auto& w : rawfiles["warnings"])
                warnings.push_back(w);
        }
        if (request.contains("weapons"))
        {
            const auto& weaponReq = request["weapons"];
            const auto entitiesJson = projectRoot / "BSP" / "entities.json";
            if (fs::exists(entitiesJson))
            {
                // T6 resolves a placed weapon at spawn and crashes on an unknown one
                std::set<std::string> staged;
                std::error_code e;
                if (fs::is_directory(projectRoot / "content_source" / "weapons", e))
                    for (const auto& entry : fs::directory_iterator(projectRoot / "content_source" / "weapons", e))
                        staged.insert(files::Utf8(entry.path().filename()));
                auto data = mod::Json(entitiesJson);
                // entities.entity_weapon: a weapon_<name> item or a turret's weaponinfo
                const auto weaponOf = [](const Json& ent) -> std::optional<std::string>
                {
                    const auto cls = ent.contains("classname") && ent["classname"].is_string() ? ent["classname"].get<std::string>() : std::string();
                    if (StartsWith(cls, "weapon_"))
                        return cls.substr(7);
                    if (cls == "misc_turret" && ent.contains("weaponinfo") && ent["weaponinfo"].is_string())
                        return ent["weaponinfo"].get<std::string>();
                    return std::nullopt;
                };
                std::set<std::string> absent;
                for (const auto& ent : data["entities"])
                    if (const auto w = weaponOf(ent); w && !w->empty() && !staged.count(*w))
                        absent.insert(*w);
                if (!absent.empty())
                {
                    Json kept = Json::array();
                    for (const auto& ent : data["entities"])
                        if (const auto w = weaponOf(ent); !w || !absent.count(*w))
                            kept.push_back(ent);
                    const auto count = data["entities"].size() - kept.size();
                    data["entities"] = kept;
                    files::WriteText(entitiesJson, py::JsonDumps(data, 1) + "\n");
                    warnings.push_back("ENTITY_WEAPON_ABSENT " + mod::ListRepr(std::vector<std::string>(absent.begin(), absent.end())) +
                                       ": no WaW zone defines them; " + std::to_string(count) + " placed entities dropped");
                }
            }
            if (weaponReq.value("staged", false))
            {
                // _stage_weapon_runtime: weapon visuals + the weapons mod.ff can carry
                const auto contentDir = projectRoot / "content_source";
                if (!techsetDump)
                    errors.push_back("weapons: no --techset-dump; weapon materials cannot be translated");
                else
                {
                    auto visuals = StageWeaponVisuals(Json{{"roots", Paths(roots)}, {"project", Str(contentDir)}, {"stock_materials", weaponReq.at("stock_materials")},
                                                           {"techset_dump", Str(*techsetDump)}, {"bo2_raw", bo2Root ? Json(Str(*bo2Root / "raw")) : Json()},
                                                           {"native_fallbacks", weaponReq.at("native_fallbacks")}, {"layered_vertex_data", weaponReq.value("layered_vertex_data", false)}},
                                                      recovery);
                    if (visuals.contains("recovered"))
                        weaponRecovered.update(visuals["recovered"]);
                    for (const auto& w : visuals["warnings"])
                        warnings.push_back(w);
                    weapons::StageShadowVariants(contentDir);
                    Json loaded = Json::object();
                    std::set<std::string> reserved;
                    if (bo2Root)
                    {
                        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
                        const auto api = gsc::port::T6Api::Load(extract::ResourceRoot() / "src/waw2bo2/compat/t6_script_api.json", stage / "t6api_cache.json");
                        for (const auto& [kind, names] : api.stockAssets)
                            loaded[kind] = std::vector<std::string>(names.begin(), names.end());
                        const auto templateZone = *bo2Root / "mods" / "zm_test" / "zm_test.zone";
                        if (fs::exists(templateZone))
                        {
                            const auto templateText = files::ReadText(templateZone); // SplitLines views into it
                            for (const auto& line : py::SplitLines(templateText))
                                if (StartsWith(line, "weapon,"))
                                {
                                    const auto field = std::string(line.substr(line.rfind(',') + 1));
                                    reserved.insert(std::string(py::Strip(field)));
                                }
                        }
                        const auto rawWeapons = *bo2Root / "raw" / "weapons";
                        if (fs::is_directory(rawWeapons))
                            for (const auto& file : files::GlobSuffix(rawWeapons, ""))
                                reserved.insert(files::Utf8(file.filename()));
                    }
                    const auto runtime = Call(StageWeaponRuntime, Json{{"content", Str(contentDir)}, {"ipak", project + "_mod"}, {"bo2_root", PathOrNull(bo2Root)},
                                                                       {"loaded", loaded}, {"converted_fx", weaponReq.at("converted_fx")},
                                                                       {"reserved", std::vector<std::string>(reserved.begin(), reserved.end())}});
                    std::string zoneLines;
                    for (const auto& line : runtime.at("zone_lines"))
                        zoneLines += line.get<std::string>() + "\n";
                    AppendZone(projectRoot, zoneLines);
                    for (const auto& n : runtime.at("notes"))
                        warnings.push_back(n);
                    std::map<std::string, Json> excluded;
                    for (const auto& [name, reasons] : runtime.at("excluded").items())
                        excluded[name] = reasons;
                    for (const auto& [name, reasons] : excluded)
                    {
                        std::string joined;
                        for (const auto& r : reasons)
                            joined += (joined.empty() ? "" : "; ") + r.get<std::string>();
                        warnings.push_back("UNSUPPORTED_WEAPON " + name + ": not carried in mod.ff: " + joined);
                    }
                    fs::create_directories(projectRoot / "maps" / "mp" / "waw");
                    const auto finished = Call(FinishWeaponRuntime, Json{{"project_root", Str(projectRoot)}, {"fx_table", report["fx_table"]}, {"waw_map", request.at("waw_map_stem")},
                                                                         {"weapons", runtime.at("table")}, {"map_table_roots", weaponReq.at("map_table_roots")},
                                                                         {"table_roots", weaponReq.at("table_roots")}});
                    content["map_tables"] = finished["map_tables"];
                    for (const auto& n : finished["wall_buy_notes"])
                        warnings.push_back(n);
                    content["weapons"]["runtime"] = runtime.at("weapons").size();
                    content["weapons"]["excluded"] = runtime.at("excluded").size();
                    content["weapons"]["visual_errors"] = visuals.at("errors").size();
                    content["weapons"]["runtime_report"] = "content_source/weapons.runtime.json";
                }
            }
        }
        if (request.contains("visuals"))
        {
            const auto& visuals = request["visuals"];
            if (!stockJson.is_null() && hasScripts)
            {
                const auto hud = Timed("Stage original WaW HUD", [&] { return Call(StageWawHud, Json{{"project_root", Str(projectRoot)}, {"roots", Paths(roots)}, {"stock_waw", stockJson}}); });
                if (hud["stock_prepared"].is_object())
                    stockJson["prepared"] = hud["stock_prepared"];
                content["waw_hud"] = hud["result"];
            }
            // shaders.stage, then what the shader runtime bound
            std::vector<std::string> rootStrings;
            for (const auto& r : roots)
                rootStrings.push_back(Str(r));
            Timed("Translate shaders", [&] { return shaders::Stage(rootStrings, projectRoot / "content_source/shaders"); });
            const auto shaderReport = mod::Json(projectRoot / "content_source/shaders/stage.json");
            Json shaderContent = Json::object();
            for (const auto& [k, v] : shaderReport.items())
                if (k != "shaders")
                    shaderContent[k] = v;
            shaderContent["report"] = "content_source/shaders/stage.json";
            Json runtimeMaterials = report["materials"];
            const auto weaponVisuals = projectRoot / "content_source/weapons.visuals.json";
            if (mod::IsFile(weaponVisuals))
            {
                const auto visualsReport = mod::Json(weaponVisuals);
                for (const auto& m : visualsReport.at("materials"))
                    runtimeMaterials.push_back(m);
            }
            std::vector<const Json*> active;
            long long activeMaterials = 0, vertexPasses = 0;
            std::set<std::string> uniqueShaders;
            for (const auto& m : runtimeMaterials)
            {
                const auto runtime = m.contains("shader_runtime") && m["shader_runtime"].is_object() ? m["shader_runtime"] : Json::object();
                const auto passes = runtime.value("active", Json::array());
                activeMaterials += Truthy(passes);
                // pointers into runtimeMaterials itself (a ternary would yield a temporary copy)
                if (m.contains("shader_runtime") && m["shader_runtime"].is_object() && m["shader_runtime"].contains("active"))
                    for (const auto& p : m["shader_runtime"]["active"])
                        active.push_back(&p);
            }
            for (const auto* p : active)
            {
                vertexPasses += Truthy(p->value("vertex_shader", Json()));
                if (Truthy(p->at("shader")))
                    uniqueShaders.insert(p->at("shader").get<std::string>());
                if (Truthy(p->value("vertex_shader", Json())))
                    uniqueShaders.insert((*p)["vertex_shader"].get<std::string>());
            }
            shaderContent["runtime"] = Json{{"active_materials", activeMaterials}, {"pixel_passes", active.size()}, {"vertex_passes", vertexPasses}, {"unique_shaders", uniqueShaders.size()}};
            content["shaders"] = shaderContent;
            if (Truthy(shaderReport.at("shaders")))
                warnings.push_back("CONTENT_INCOMPLETE shaders: " + std::to_string(shaderReport.at("translated").get<long long>()) + " translated to SM5; " +
                                   std::to_string(shaderReport.at("unsupported").get<long long>()) + " translation failures; " + std::to_string(active.size()) +
                                   " runtime pixel passes bound; remaining programs require supported T6 pass adapters");
            // No loose raw/ visions: WaW reads visions only from loaded zones (vision/default when missing)
            std::vector<fs::path> iwds;
            for (const auto& d : PathList(visuals.at("iwd_dirs")))
                for (const auto& f : files::GlobSuffix(d, ".iwd"))
                    if (f.parent_path() == d && std::find(iwds.begin(), iwds.end(), f) == iwds.end())
                        iwds.push_back(f);
            std::sort(iwds.begin(), iwds.end(), [](const fs::path& a, const fs::path& b) { return mod::Lower(Str(a)) < mod::Lower(Str(b)); });
            const auto vision = Timed("Convert vision and glow", [&] {
                return Call(visions::StageVisions, Json{{"project", Str(projectRoot)}, {"roots", visuals.at("vision_roots")}, {"iwds", Paths(iwds)},
                                                         {"stock", stockJson.is_null() ? Json() : Json{{"waw_root", stockJson["waw_root"]}, {"unlinker", stockJson["unlinker"]}, {"work", stockJson["work"]}}},
                                                         {"donor", nullptr}, {"source_root", nullptr}, {"techset_dump", PathOrNull(techsetDump)}});
            });
            for (const auto& note : vision.value("numeric_prefixes", Json::array()))
            {
                char parsed[64];
                std::snprintf(parsed, sizeof parsed, "%g", note.at("parsed").get<double>());
                const auto& original = note.at("original");
                mod::Print("WARNING: vision " + note.at("vision").get<std::string>() + ": " + note.at("field").get<std::string>() + " " +
                           (original.is_string() ? py::StringRepr(original.get<std::string>()) : original.dump()) + " read as " + parsed +
                           " using WaW numeric-prefix parsing (" + note.at("source").get<std::string>() + ")");
            }
            for (const auto& e : vision.at("errors"))
                errors.push_back(e);
            const auto overlay = vision.contains("overlay") && vision["overlay"].is_object() ? vision["overlay"] : Json::object();
            for (const auto& name : overlay.value("materials", Json::array()))
            {
                report["materials"].push_back(Json{{"name", name}, {"file", mod::Generic(assets::MaterialPath(name.get<std::string>()))},
                                                   {"t6_techset", "waw film/glow overlay"}, {"notes", Json::array({"WaW film + glow pass (glow.py)"})}});
                request["hud_materials"].push_back(name);
            }
            const auto overlayNotes = overlay.value("notes", Json::object());
            for (const auto& [name, notes] : overlayNotes.items())
                for (const auto& note : notes)
                    warnings.push_back("GLOW " + name + ": " + note.get<std::string>());
            for (const auto& name : vision.at("missing"))
                warnings.push_back("VISION_ABSENT " + name.get<std::string>() + ": absent from WaW lookup");
            for (const auto& name : vision.value("default_fallback", Json::array()))
                warnings.push_back("VISION_DEFAULT " + name.get<std::string>() + ": in no WaW zone; WaW loads vision/default instead, so does the port");
            if (Truthy(vision.at("visions")))
                images.push_back("waw_vision_lut");
            request["sounds"]["vision_report"] = vision;
            request["lighting"]["script_render_dvars"] = vision.value("script_render_dvars", Json::object());
        }
        if (request.contains("client"))
        {
            // water bullets, the environment script and the client mains
            const auto& client = request["client"];
            const auto runtimeFile = projectRoot / "content_source/weapons.runtime.json";
            const auto weaponTable = mod::IsFile(runtimeFile) ? mod::Json(runtimeFile).at("table") : Json::object();
            const auto water = Call(weapons::StageWaterBulletsRequest, Json{{"project", Str(projectRoot)}, {"hasWater", client.at("has_water")}, {"roots", Paths(roots)}, {"weapons", weaponTable}});
            if (water.contains("warning"))
                warnings.push_back(water["warning"]);
            if (water.contains("content"))
                content["water_bullets"] = water["content"];
            std::set<std::string> soundNames;
            for (const char* key : {"zone_sounds", "compiled_sounds"})
                for (const auto& n : request.at("sounds").at(key))
                    soundNames.insert(n.get<std::string>());
            if (!stockJson.is_null())
            {
                const auto stock = extract::StockWaw::FromJson(stockJson);
                for (const auto& [_, kinds] : stock->Index().items())
                    if (kinds.contains("sound"))
                        for (const auto& n : kinds["sound"])
                            soundNames.insert(n.get<std::string>());
            }
            const auto mapClient = report["scripts"].is_object() && report["scripts"].contains("client") ? report["scripts"]["client"] : Json();
            const auto finished = Call(FinishClientScripts, Json{{"project_root", Str(projectRoot)}, {"project", project}, {"roots", Paths(roots)}, {"fx_table", report["fx_table"]},
                                                                 {"sound_names", std::vector<std::string>(soundNames.begin(), soundNames.end())},
                                                                 {"characters", report["scripts"].is_object() && Truthy(report["scripts"].value("characters", Json()))},
                                                                 {"map_client", mapClient}});
            if (finished.contains("value_error"))
                throw StepError("ValueError", finished["value_error"].get<std::string>());
            if (Truthy(mapClient))
                report["scripts"]["client"] = finished["map_client"];
            for (const auto& w : finished["warnings"])
                warnings.push_back(w);
        }
        if (request.contains("sounds"))
            StageBridgeSound(request, report, stockJson);
        if (request.contains("lighting"))
        {
            // the WaW light grid, the sun, primary lights and their definitions
            const auto& lighting = request["lighting"];
            const auto base = lighting.at("base").get<std::string>();
            const auto gridFile = files::FromUtf8(base + ".lightgrid.bin");
            if (fs::exists(gridFile))
            {
                const auto modelsFile = projectRoot / "BSP/models.json";
                Json gridRequest{{"source", Str(gridFile)}, {"destination", Str(projectRoot / "BSP/lightgrid.bin")}, {"compensated", true}};
                if (fs::exists(modelsFile))
                {
                    gridRequest["models"] = Str(modelsFile);
                    gridRequest["clip"] = lighting.at("clip_source");
                    gridRequest["primary_lights"] = Str(files::FromUtf8(base + ".primarylights.json"));
                }
                content["lighting"] = Timed("Bake static model lighting (WaW grid traces)", [&] { return Call(world::StageGrid, gridRequest); });
                const auto modelLighting = content["lighting"].contains("static_model_lighting") && content["lighting"]["static_model_lighting"].is_object()
                                               ? content["lighting"]["static_model_lighting"] : Json::object();
                if (Truthy(modelLighting.value("no_corner_reached", Json())))
                    warnings.push_back("STATIC_MODEL_GRID_FALLBACK " + std::to_string(modelLighting["no_corner_reached"].get<long long>()) +
                                       " static models reach no light grid corner: sun at visibility 255, palette entry 1 (0 below the grid), as WaW sub_71C360");
                if (Truthy(modelLighting.value("outside_grid", Json())))
                    warnings.push_back(std::to_string(modelLighting["outside_grid"].get<long long>()) + " static models lie outside the WaW light grid: lit by palette entry 0");
                const auto sunFile = files::FromUtf8(base + ".lighting.json");
                if (fs::exists(sunFile))
                {
                    fs::copy_file(sunFile, projectRoot / "BSP/lighting.json", fs::copy_options::overwrite_existing);
                    fs::last_write_time(projectRoot / "BSP/lighting.json", fs::last_write_time(sunFile));
                    // the values a map script sets on the r_lightTweak* dvars are the sun WaW draws
                    const std::vector<std::pair<std::string, std::string>> sunDvars{
                        {"r_lighttweaksunlight", "sunLight"}, {"r_lighttweakambient", "ambientScale"}, {"r_lighttweakdiffusefraction", "diffuseFraction"}};
                    Json scriptValues = Json::object();
                    const auto dvars = lighting.value("script_render_dvars", Json::object());
                    for (const auto& [k, v] : dvars.items())
                        for (const auto& [dvar, field] : sunDvars)
                            if (k == dvar)
                            {
                                double value = 0;
                                if (v.is_number())
                                    value = v.get<double>();
                                else if (!py::ParseFloat(v.get<std::string>(), value))
                                    throw StepError("ValueError", "could not convert string to float: " + py::StringRepr(v.get<std::string>()));
                                scriptValues[field] = value;
                            }
                    if (!scriptValues.empty())
                    {
                        auto staged = mod::Json(projectRoot / "BSP/lighting.json");
                        for (const auto& [k, v] : scriptValues.items())
                            staged["sun"][k] = v;
                        files::WriteText(projectRoot / "BSP/lighting.json", py::JsonDumps(staged, 2) + "\n");
                        content["lighting_script_sun"] = scriptValues;
                    }
                }
                const auto primaryFile = files::FromUtf8(base + ".primarylights.json");
                if (!fs::exists(primaryFile))
                    errors.push_back("source LightGrid requires its matching primary-light table");
                else
                {
                    const auto primary = world::StagePrimaryLights(primaryFile, projectRoot / "BSP/primarylights.json", lighting.value("shadow_lights", false));
                    if (primary.contains("error"))
                        throw StepError("LightingError", primary["error"].get<std::string>());
                    if (!primary["unshadowed"].empty())
                        warnings.push_back(std::to_string(primary["unshadowed"].size()) +
                                           " primary spot/omni lights drawn without shadow maps (render dump has no shadow geometry: re-extract gfxworld with the v6 T4 dumper)");
                    std::set<std::string> definitions;
                    for (const auto& d : primary["definitions"])
                        definitions.insert(d.get<std::string>());
                    const auto lightdefs = Call(StageLightDefs, Json{{"project_root", Str(projectRoot)}, {"roots", Paths(roots)},
                                                                     {"definitions", std::vector<std::string>(definitions.begin(), definitions.end())}});
                    for (const auto& i : lightdefs["images"])
                        images.push_back(i);
                    for (const auto& e : lightdefs["errors"])
                        errors.push_back(e);
                }
                for (const char* suffix : {".lightgrid.bin", ".lighting.json", ".primarylights.json"})
                {
                    const auto original = files::FromUtf8(base + suffix);
                    if (fs::exists(original))
                    {
                        const auto target = projectRoot / "content_source/lighting" / original.filename();
                        fs::create_directories(target.parent_path());
                        fs::copy_file(original, target, fs::copy_options::overwrite_existing);
                        fs::last_write_time(target, fs::last_write_time(original));
                    }
                }
                fs::create_directories(projectRoot / "content_source/lighting");
                const auto pages = content.contains("lightmaps") && content["lightmaps"].contains("pages") ? content["lightmaps"]["pages"].size() : 0;
                warnings.push_back("WaW LightGrid staged with " + std::to_string(pages) +
                                   " lightmap pages; T4 corner obstruction traces require a separate runtime adapter because T6 stores scalar visibility");
            }
            // the window barrier
            const auto barrier = Call(StageZbarrier, Json{{"project_root", Str(projectRoot)}, {"techset_dump", PathOrNull(techsetDump)}, {"bo2_root", PathOrNull(bo2Root)},
                                                          {"timing", report["scripts"].is_object() && report["scripts"].contains("window_repair_timing") ? report["scripts"]["window_repair_timing"] : Json()}});
            for (const auto& w : barrier["warnings"])
                warnings.push_back(w);
            for (const auto& e : barrier["errors"])
                errors.push_back(e);
            for (const auto& [k, v] : barrier["content"].items())
                content[k] = v;
        }

        // stage_model_overlay_fx
        const auto overlays = Call(effects::StageModelOverlayRuntimeRequest, Json{{"project", Str(projectRoot)}, {"effects", report["fx_table"]}});
        content["model_overlay_fx"] = overlays;
        std::vector<std::string> overlayVariants;
        for (const auto& entry : overlays)
            overlayVariants.push_back(entry.at("variant").get<std::string>());
        // modelisolation.stage
        const auto modelNames = Call(models::IsolateModelNamesRequest, Json{{"project", Str(projectRoot)}});
        content["model_identities"] = modelNames;
        const auto scripts = Call(gsc::port::MapScriptsRequest, Json{{"root", Str(projectRoot)}, {"project", project}});
        if (hasScripts && bo2Root)
        {
            // Validate the completed graph, including owned perk/bootstrap modules
            const auto link = Call(LinkCheckStep, Json{{"project_root", Str(projectRoot)}, {"api_cache", Str(stage / "t6api_cache.json")}});
            for (const auto& e : link["problems"])
                errors.push_back("scripts: link: " + e.get<std::string>());
        }
        const auto rawRoots = PathList(request.at("raw_roots"));
        bool menuReady = false;
        if (!stockJson.is_null() && hasScripts && wawMapStem)
        {
            // WaW's own pause menu (ui/hud.txt + ui/ingame.txt) replaces BO2's class menu
            std::error_code e;
            fs::remove(projectRoot / "content_source/waw_menu.json", e);
            const auto staged = Timed("Translate WaW in-game menus", [&] { return Call(StageWawMenus, Json{{"project_root", Str(projectRoot)}, {"raw_roots", Paths(rawRoots)}, {"mapname", *wawMapStem}}); });
            if (staged.contains("menu_error"))
                warnings.push_back("FRAMEWORK_BO2 pause menu: " + staged["menu_error"].get<std::string>() + "; BO2 pause menu kept");
            else
            {
                content["waw_menu"] = staged["result"];
                menuReady = true;
            }
        }
        // localization.stage: localize-only zone dumps live in the shared WaW cache
        const auto localizationDumps = !stockJson.is_null() ? files::FromUtf8(stockJson.at("work").get<std::string>()) / "localization_dumps" : stage / "localization_dumps";
        const Json localizationRequest{{"project", Str(projectRoot)},
                                       {"roots", Paths(roots)},
                                       {"iwd_dirs", request.at("iwd_dirs")},
                                       {"unlinker", request.at("t4_unlinker")},
                                       {"work", Str(localizationDumps)},
                                       {"raw_roots", Paths(rawRoots)},
                                       {"bo2_root", PathOrNull(bo2Root)},
                                       {"language", "english"},
                                       {"t6_unlinker", request.at("t6_unlinker")},
                                       {"t6_stock_dump", PathOrNull(techsetDump)},
                                       {"stock", stockJson.is_null() ? Json() : Json{{"waw_root", stockJson["waw_root"]}, {"unlinker", stockJson["unlinker"]}, {"work", stockJson["work"]}}}};
        const auto localization = Timed("Stage localization", [&] { return Call(strings::StageLocalization, localizationRequest); });
        content["localization"] = localization;
        Json recovered = Json::object();
        if (menuReady)
        {
            Json localized = Json::object();
            for (const auto& entry : localization.value("entries", Json::array()))
                localized[entry.at("key").get<std::string>()] = entry.at("value");
            auto art = Timed("Stage WaW menu art", [&] {
                return StageWawMenuArt(Json{{"project_root", Str(projectRoot)}, {"roots", Paths(roots)}, {"localized", localized}, {"stock_waw", stockJson}}, recovery);
            });
            if (art.contains("stock_prepared") && art["stock_prepared"].is_object())
            {
                stockJson["prepared"] = art["stock_prepared"];
                art.erase("stock_prepared");
            }
            if (art.contains("recovered"))
            {
                recovered.update(art["recovered"]);
                art.erase("recovered");
            }
            for (const auto& [k, v] : art.items())
                content["waw_menu"][k] = v;
            for (const auto& name : art["missing"])
                warnings.push_back("WAW_MENU_ASSET_MISSING " + name.get<std::string>() + ": drawn empty");
        }
        for (const auto& e : localization["errors"])
            errors.push_back(e);
        for (const auto& w : localization["warnings"])
            warnings.push_back(w);
        {
            std::string lines = "localize," + localization["asset"].get<std::string>() + "\n";
            if (request.value("bo2_stock_perks", false))
            {
                lines += "// Opt-in stock BO2 perk dependencies\n";
                for (const auto& line : PerkAssetLines(projectRoot))
                    lines += line + "\n";
            }
            AppendZone(projectRoot, lines);
        }
        // modelcolours.stage_surface_fixes
        {
            const auto fixes = Call(models::StageSurfaceFixesRequest, Json{{"project", Str(projectRoot)}, {"materials", report["materials"]}, {"colors", true}, {"sort", true}});
            const auto& opaque = fixes["opacity"];
            content["additive_model_vertex_alpha"] = opaque;
            if (Truthy(opaque))
            {
                long long vertices = 0;
                for (const auto& [_, n] : opaque.items())
                    vertices += n.get<long long>();
                warnings.push_back("ADDITIVE_MODEL_VERTEX_ALPHA " + std::to_string(vertices) + " vertices in " + std::to_string(opaque.size()) +
                                   " models: mc_unlit_add surfaces draw their vertex colour at alpha 1");
            }
            const auto& reordered = fixes["sortKeys"];
            content["model_surface_sort_keys"] = reordered;
            if (Truthy(reordered))
            {
                std::map<std::string, Json> sorted;
                for (const auto& [n, ab] : reordered.items())
                    sorted[n] = ab;
                std::string text;
                for (const auto& [n, ab] : sorted)
                    text += (text.empty() ? "" : ", ") + n + " " + std::to_string(ab.at(0).get<long long>()) + "->" + std::to_string(ab.at(1).get<long long>());
                warnings.push_back("MODEL_SURFACE_SORT_ORDER " + text + ": same WaW sort key, T6 key raised to keep the model surface order");
            }
        }
        // materialisolation.stage
        Json materialNames = Json::object();
        if (techsetDump)
        {
            Json entries = Json::array();
            for (const auto& entry : report["materials"])
                entries.push_back(Json{{"name", entry.at("name")}, {"file", entry.at("file")}});
            const auto hud = report["scripts"].is_object() && report["scripts"].contains("hud_materials") ? report["scripts"]["hud_materials"] : Json::array();
            const auto result = Call(models::IsolateMaterialNamesRequest, Json{{"project", Str(projectRoot)}, {"stock", Str(*techsetDump)}, {"materials", entries}, {"hud", hud}});
            for (auto update : result["updates"])
            {
                const auto index = update.at("index").get<size_t>();
                update.erase("index");
                for (const auto& [k, v] : update.items())
                    report["materials"][index][k] = v;
            }
            materialNames = result["renamed"];
            if (Truthy(materialNames))
                warnings.push_back("MATERIAL_IDENTITIES_ISOLATED " + std::to_string(materialNames.size()) + " converted materials shared stock BO2 names");
        }
        content["material_identities"] = materialNames;
        // constraints.stage_runtime
        const auto ropes = Call(physics::Run, Json{{"operation", "rope-runtime"}, {"project", Str(projectRoot)}});
        if (ropes.contains("invalid"))
            throw StepError("ValueError", ropes["invalid"].get<std::string>());
        const auto ropeRuntime = ropes.at("result");
        for (const auto& joint : ropeRuntime.value("unsupported_joints", Json::array()))
            warnings.push_back("MAP_JOINT_UNSUPPORTED " + joint.get<std::string>() + ": T6 has no map entity for it; the body hangs by its ropes only");
        content["physics_constraint_runtime"] = ropeRuntime;
        std::set<std::string> hudMaterials;
        for (const auto& name : request.at("hud_materials"))
        {
            const auto n = name.get<std::string>();
            hudMaterials.insert(materialNames.contains(n) ? materialNames[n].get<std::string>() : n);
        }
        std::set<std::string> staged;
        for (const auto& m : report["materials"])
            staged.insert(m.at("name").get<std::string>());
        // rumbles.stage
        auto rumbleRoots = roots;
        rumbleRoots.insert(rumbleRoots.end(), rawRoots.begin(), rawRoots.end());
        const auto rumbles = Call(StageRumblesRequest, Json{{"project", Str(projectRoot)}, {"roots", Paths(rumbleRoots)}, {"nativeRaw", bo2Root ? Str(*bo2Root / "raw") : std::string()}});
        if (rumbles.contains("invalid"))
            throw StepError("ValueError", rumbles["invalid"].get<std::string>());
        content["rumbles"] = rumbles;
        for (const auto& name : rumbles["missing"])
            errors.push_back("RUMBLE_DEPENDENCY_MISSING " + name.get<std::string>());
        // write_zone
        {
            // sorted(model_names.get(m, m) for m in ...): two models may share an identity
            std::vector<std::string> xmodels;
            for (const auto& m : request.at("zone_models"))
            {
                const auto n = m.get<std::string>();
                xmodels.push_back(modelNames.contains(n) ? modelNames[n].get<std::string>() : n);
            }
            std::sort(xmodels.begin(), xmodels.end());
            Json effects = request.at("zone_fx");
            for (const auto& v : overlayVariants)
                effects.push_back(v);
            Json materials = Json::array();
            for (const auto& m : hudMaterials)
                if (staged.count(m))
                    materials.push_back(m);
            for (const auto& m : ropeRuntime.value("materials", Json::array()))
                materials.push_back(m);
            const Json assets{{"image", images},
                              {"xmodel", std::vector<std::string>(xmodels.begin(), xmodels.end())},
                              {"zbarrier", Json::array()},
                              {"fx", effects},
                              {"material", materials},
                              {"script", scripts},
                              {"localize", Json::array({localization["asset"]})},
                              {"rawfile", rumbles["files"]}};
            Call(world::WriteMapZoneRequest, Json{{"stage", Str(stage)}, {"project", project}, {"assets", assets}, {"ipak", request.value("ipak", true)}});
        }
        // CLAUDE.md: every function BO2 runs in place of the map's own without a glue reason is reported
        const auto covered = Call(FrameworkCoverage, Json{{"project", project}, {"project_root", Str(projectRoot)}, {"roots", request.at("coverage_roots")}, {"scripts", report["scripts"]}});
        content["framework_coverage"] = covered["totals"];
        for (const auto& w : covered["warnings"])
            warnings.push_back(w);
        mod::Print("framework coverage: " + CountsRepr(covered["totals"]) + " (framework_coverage.md)");
        files::WriteText(projectRoot / "bridge_stage.report.json", py::JsonDumps(report, 2) + "\n");
        weaponRecovered.update(recovered);
        return Json{{"report", report}, {"stock_prepared", stockJson.is_null() ? Json() : stockJson.value("prepared", Json())}, {"recovered", weaponRecovered}};
    }
} // namespace w2b::pipeline
