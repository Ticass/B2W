// The end of the bridge stage (see bridge_tail.h).
#include "bridge_tail.h"
#include "finish.h"
#include "mapents/map_entity.h"
#include "bridge_sound.h"
#include "bridge_support.h"

#include "core/regex.h"
#include "coverage_stage.h"
#include "effects/stage.h"
#include "extract/stockwaw.h"
#include "gsc/port/staging.h"
#include "hud_stage.h"
#include "hooks_stage.h"
#include "extract/cache.h"
#include "gsc/port/api.h"
#include "gsc/port/portmap.h"
#include "gsc/port/t6api.h"
#include "gsc/port/zone_scripts.h"
#include "weapon_runtime.h"
#include "weapon_visuals.h"
#include "weapons/stage.h"
#include "core/asset_paths.h"
#include "shaders/stage.h"
#include "shaders/techsets.h"
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
    using bridge::OptionalPath;
    using bridge::PathList;
    using bridge::PathOrNull;
    using bridge::Paths;
    using bridge::Timed;
    using bridge::Truthy;
    using mod::StartsWith;
    using mod::Str;
    using util::Json;

    namespace
    {
        // stockperks.asset_lines: explicit native perk dependencies
        std::vector<std::string> PerkAssetLines(const fs::path& outRoot)
        {
            static const regex::Regex call("\\b(precacheitem|precachemodel|precacheshader|loadfx)\\s*\\(\\s*\"([^\"\\n]+)\"", regex::IGNORECASE);
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

        std::vector<models::SurfaceMaterial> SurfaceView(const std::vector<MaterialRecord>& records)
        {
            std::vector<models::SurfaceMaterial> view;
            for (const auto& record : records)
                view.push_back({record.WawName(), record.file, record.t4Techset && *record.t4Techset ? **record.t4Techset : std::string(),
                                record.t4SortKey ? std::optional(static_cast<int>(*record.t4SortKey)) : std::nullopt});
            return view;
        }

        // repr() of a dict of str -> int, as print() shows framework_coverage.totals
        std::string CountsRepr(const std::vector<std::pair<std::string, long long>>& counts)
        {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : counts)
            {
                out += (first ? "" : ", ") + util::StringRepr(k) + ": " + std::to_string(v);
                first = false;
            }
            return out + "}";
        }
    } // namespace

    Json BridgeTail(TailInput input, BridgeReport& report, extract::StockWaw* stock, textures::IwdRecovery& recovery)
    {
        // input: the visuals block supplies the later blocks' vision report and HUD materials
        const auto& project = input.project;
        const auto& projectRoot = input.projectRoot;
        const auto& stage = input.stage;
        const auto& roots = input.roots;
        const auto& bo2Root = input.bo2Root;
        const auto& techsetDump = input.techsetDump;
        const auto& wawMapStem = input.wawMapStem;

        const bool hasScripts = Truthy(report.scripts);
        auto& warnings = report.warnings;
        auto& errors = report.errors;
        auto& content = report.content;
        const auto characters = report.scripts.is_object() && Truthy(report.scripts.value("characters", Json()));
        auto& images = input.images;
        Json weaponRecovered = Json::object();
        {
            const auto& scripts = input.scripts;
            // verify_techsets: every technique set the materials use, with its shaders
            {
                const auto& used = scripts.usedTechsets;
                report.techsets.assign(used.begin(), used.end());
                if (!techsetDump)
                    errors.push_back("no --techset-dump given; the bridge cannot load technique sets without techniquesets/*.json + shader_bin/*.cso dumped from a stock T6 zone");
                else
                {
                    const std::vector<fs::path> lookup{projectRoot, *techsetDump};
                    const auto find = [&](const fs::path& rel) -> std::optional<fs::path>
                    {
                        std::error_code e;
                        for (const auto& root : lookup)
                            if (fs::exists(root / rel, e))
                                return root / rel;
                        return std::nullopt;
                    };
                    const auto shadersOf = [](const fs::path& techset) { return shaders::ProgramFiles(shaders::ReadT6TechniqueSet(techset)); };
                    for (const auto& name : used)
                    {
                        const fs::path rel = fs::path("techniquesets") / files::FromUtf8(name + ".json");
                        auto path = find(rel);
                        if (!path)
                        {
                            // outside the donor view: copy it and its shaders from its own zone
                            const auto owner = extract::RequiredBo2Techset(*techsetDump, name);
                            if (owner)
                            {
                                std::vector<std::string> rels{mod::Generic(rel)};
                                for (const auto& sh : shadersOf(*owner / rel))
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
                        for (const auto& sh : shadersOf(*path))
                            if (!find(files::FromUtf8(sh)))
                                errors.push_back("technique set " + name + ": shader " + sh + " missing from dump");
                    }
                }
            }
            // stage_map_scripts: the template's scripts under the project name, the map's main
            const std::optional<fs::path> mapScript = scripts.wawMapScript;
            gsc::port::StageDiagnostics diagnostics;
            if (bo2Root)
                gsc::port::StageTemplateScripts(projectRoot, project, {*bo2Root / "mods" / "zm_test", "zm_test"}, diagnostics);
            gsc::port::StageRequiredScripts(projectRoot, project, scripts.requiredTemplate, diagnostics);
            gsc::port::RewriteTemplateReferences(projectRoot, project, diagnostics);
            for (const auto& e : diagnostics.errors)
                errors.push_back(e);
            for (const auto& w : diagnostics.warnings)
                warnings.push_back(w);
            const auto mainGsc = projectRoot / "maps" / "mp" / files::FromUtf8(project + ".gsc");
            const bool hasMain = fs::exists(mainGsc);
            std::optional<gsc::port::ZoneMainResult> mainResult;
            if (scripts.dropZoneManager && hasMain)
                mainResult = gsc::port::StageZoneMain(mainGsc, *scripts.dropZoneManager, true);
            if (!mapScript || !hasMain)
                errors.push_back("no WaW map script given; the BO2 map script would keep the template's single zone");
            if (hasMain)
            {
                auto graph = scripts.zoneGraph;
                if (!graph && mapScript)
                    graph = gsc::port::ReadZoneGraphFile(*mapScript);
                if (!mainResult)
                    mainResult = gsc::port::StageZoneMain(mainGsc, mapScript.has_value(), true);
                if (!mainResult->error.empty())
                    errors.push_back("zones: " + mainResult->error);
                else if (mapScript)
                    warnings.push_back("zone graph from " + files::Utf8(mapScript->filename()) + ": start zones " + mod::ListRepr(graph->initial) + ", " +
                                       std::to_string(graph->adjacency.size()) + " links");
                if (mainResult->guardedSwitch)
                    warnings.push_back("zm_test electric_switch guarded: WaW map has no template switch entities");
            }
            MapHookRequest hooks{projectRoot, project, wawMapStem.value_or(std::string()), bo2Root, input.bo2StockPerks, hasScripts,
                                 characters, {}};
            hooks.stagedAnimtrees = report.Script("staged_animtrees").get<std::vector<std::string>>();
            HookMapScripts(hooks).MergeInto(report);
            // stage_rawfiles
            gsc::port::StageDiagnostics rawfiles;
            gsc::port::StageScriptRawfiles(projectRoot, bo2Root, rawfiles);
            for (const auto& e : rawfiles.errors)
                errors.push_back(e);
            for (const auto& w : rawfiles.warnings)
                warnings.push_back(w);
        }
        {
            const auto& weaponReq = input.weapons;
            const auto entitiesJson = projectRoot / "BSP" / "entities.json";
            if (fs::exists(entitiesJson))
            {
                // T6 resolves a placed weapon at spawn and crashes on an unknown one
                std::set<std::string> staged;
                std::error_code e;
                if (fs::is_directory(projectRoot / "content_source" / "weapons", e))
                    for (const auto& entry : fs::directory_iterator(projectRoot / "content_source" / "weapons", e))
                        staged.insert(files::Utf8(entry.path().filename()));
                auto entities = mapents::ReadMapEntities(entitiesJson);
                // entities.entity_weapon: a weapon_<name> item or a turret's weaponinfo
                const auto weaponOf = [](const mapents::MapEntity& ent) -> std::optional<std::string>
                {
                    const auto cls = std::string(ent.Get("classname"));
                    if (StartsWith(cls, "weapon_"))
                        return cls.substr(7);
                    if (const auto* info = ent.Find("weaponinfo"); cls == "misc_turret" && info)
                        return *info;
                    return std::nullopt;
                };
                std::set<std::string> absent;
                for (const auto& ent : entities)
                    if (const auto w = weaponOf(ent); w && !w->empty() && !staged.count(*w))
                        absent.insert(*w);
                if (!absent.empty())
                {
                    const auto count = std::erase_if(entities, [&](const mapents::MapEntity& ent) {
                        const auto w = weaponOf(ent);
                        return w && absent.count(*w);
                    });
                    mapents::WriteMapEntities(entitiesJson, entities);
                    warnings.push_back("ENTITY_WEAPON_ABSENT " + mod::ListRepr(std::vector<std::string>(absent.begin(), absent.end())) +
                                       ": no WaW zone defines them; " + std::to_string(count) + " placed entities dropped");
                }
            }
            if (weaponReq.staged)
            {
                // _stage_weapon_runtime: weapon visuals + the weapons mod.ff can carry
                const auto contentDir = projectRoot / "content_source";
                if (!techsetDump)
                    errors.push_back("weapons: no --techset-dump; weapon materials cannot be translated");
                else
                {
                    const WeaponVisualsRequest visualsRequest{roots, contentDir, weaponReq.stockMaterials, *techsetDump,
                                                              bo2Root ? std::optional(*bo2Root / "raw") : std::nullopt, weaponReq.nativeFallbacks,
                                                              weaponReq.layeredVertexData};
                    const auto visuals = StageWeaponVisuals(visualsRequest, recovery);
                    weaponRecovered.update(visuals.recovered);
                    for (const auto& w : visuals.warnings)
                        warnings.push_back(w);
                    weapons::StageShadowVariants(contentDir);
                    WeaponRuntimeRequest runtimeRequest{contentDir, project + "_mod", bo2Root, {}, weaponReq.convertedFx, {}};
                    auto& reserved = runtimeRequest.reserved;
                    if (bo2Root)
                    {
                        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
                        const auto api = gsc::port::T6Api::Load(extract::ResourceRoot() / "src/waw2bo2/compat/t6_script_api.json", stage / "t6api_cache.json");
                        for (const auto& [kind, names] : api.stockAssets)
                            runtimeRequest.loaded[kind] = {names.begin(), names.end()};
                        const auto templateZone = *bo2Root / "mods" / "zm_test" / "zm_test.zone";
                        if (fs::exists(templateZone))
                        {
                            const auto templateText = files::ReadText(templateZone); // SplitLines views into it
                            for (const auto& line : util::SplitLines(templateText))
                                if (StartsWith(line, "weapon,"))
                                {
                                    const auto field = std::string(line.substr(line.rfind(',') + 1));
                                    reserved.insert(std::string(util::Strip(field)));
                                }
                        }
                        const auto rawWeapons = *bo2Root / "raw" / "weapons";
                        if (fs::is_directory(rawWeapons))
                            for (const auto& file : files::GlobSuffix(rawWeapons, ""))
                                reserved.insert(files::Utf8(file.filename()));
                    }
                    const auto runtime = StageWeaponRuntime(runtimeRequest);
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
                    const auto finished = FinishWeaponRuntime({projectRoot, report.fxTable,
                                                               wawMapStem, runtime.at("table").get<std::map<std::string, std::string>>(),
                                                               weaponReq.mapTableRoots, weaponReq.tableRoots});
                    content["map_tables"] = finished.mapTables;
                    for (const auto& n : finished.wallBuyNotes)
                        warnings.push_back(n);
                    content["weapons"]["runtime"] = runtime.at("weapons").size();
                    content["weapons"]["excluded"] = runtime.at("excluded").size();
                    content["weapons"]["visual_errors"] = visuals.errors;
                    content["weapons"]["runtime_report"] = "content_source/weapons.runtime.json";
                }
            }
        }
        {
            if (stock && hasScripts)
                content["waw_hud"] = Timed("Stage original WaW HUD", [&] { return StageWawHud(projectRoot, roots, stock); });
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
            auto runtimeMaterials = report.materials;
            const auto weaponVisuals = projectRoot / "content_source/weapons.visuals.json";
            if (mod::IsFile(weaponVisuals))
            {
                const auto visualsReport = mod::Json(weaponVisuals);
                for (const auto& m : visualsReport.at("materials"))
                    runtimeMaterials.push_back(MaterialRecord::FromJson(m));
            }
            std::vector<const Json*> active;
            long long activeMaterials = 0, vertexPasses = 0;
            std::set<std::string> uniqueShaders;
            for (const auto& m : runtimeMaterials)
            {
                if (!m.shaderRuntime.is_object() || !m.shaderRuntime.contains("active"))
                    continue;
                // pointers into runtimeMaterials itself
                const auto& passes = m.shaderRuntime["active"];
                activeMaterials += Truthy(passes);
                for (const auto& p : passes)
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
            for (const auto& d : input.visionIwdDirs)
                for (const auto& f : files::GlobSuffix(d, ".iwd"))
                    if (f.parent_path() == d && std::find(iwds.begin(), iwds.end(), f) == iwds.end())
                        iwds.push_back(f);
            std::sort(iwds.begin(), iwds.end(), [](const fs::path& a, const fs::path& b) { return mod::Lower(Str(a)) < mod::Lower(Str(b)); });
            const auto vision = Timed("Convert vision and glow", [&] {
                return visions::StageVisions({projectRoot, input.visionRoots, iwds, stock, std::nullopt, std::nullopt, techsetDump});
            });
            for (const auto& note : vision.value("numeric_prefixes", Json::array()))
            {
                char parsed[64];
                std::snprintf(parsed, sizeof parsed, "%g", note.at("parsed").get<double>());
                const auto& original = note.at("original");
                mod::Print("WARNING: vision " + note.at("vision").get<std::string>() + ": " + note.at("field").get<std::string>() + " " +
                           (original.is_string() ? util::StringRepr(original.get<std::string>()) : original.dump()) + " read as " + parsed +
                           " using WaW numeric-prefix parsing (" + note.at("source").get<std::string>() + ")");
            }
            for (const auto& e : vision.at("errors"))
                errors.push_back(e);
            const auto overlay = vision.contains("overlay") && vision["overlay"].is_object() ? vision["overlay"] : Json::object();
            for (const auto& name : overlay.value("materials", Json::array()))
            {
                MaterialRecord record;
                record.name = name.get<std::string>();
                record.file = mod::Generic(assets::MaterialPath(record.name));
                record.t6Techset = "waw film/glow overlay";
                record.notes = std::vector<std::string>{"WaW film + glow pass"};
                report.materials.push_back(std::move(record));
                input.hudMaterials.push_back(name.get<std::string>());
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
            input.sounds.visionReport = vision;
            input.lighting.scriptRenderDvars = vision.value("script_render_dvars", Json::object());
        }
        {
            // water bullets, the environment script and the client mains

            const auto runtimeFile = projectRoot / "content_source/weapons.runtime.json";
            std::vector<std::string> weaponFiles;
            if (mod::IsFile(runtimeFile))
            {
                const auto runtimeReport = mod::Json(runtimeFile);
                for (const auto& [_, file] : runtimeReport.at("table").items())
                    weaponFiles.push_back(file.get<std::string>());
            }
            const auto water = weapons::StageWaterBullets(projectRoot, roots, input.hasWater, weaponFiles);
            if (water.warning)
                warnings.push_back(*water.warning);
            if (water.rows)
                content["water_bullets"] = water.rows->ToJson();
            std::set<std::string> soundNames(input.sounds.zoneSounds.begin(), input.sounds.zoneSounds.end());
            soundNames.insert(input.sounds.compiledSounds.begin(), input.sounds.compiledSounds.end());
            if (stock)
            {
                for (const auto& [_, kinds] : stock->Index().items())
                    if (kinds.contains("sound"))
                        for (const auto& n : kinds["sound"])
                            soundNames.insert(n.get<std::string>());
            }
            const auto mapClient = report.Script("client", Json());
            const auto finished = FinishClientScripts({projectRoot, project, roots, report.fxTable, soundNames, characters, mapClient});
            if (Truthy(mapClient))
                report.scripts["client"] = finished.mapClient;
            for (const auto& w : finished.warnings)
                warnings.push_back(w);
        }
        StageBridgeSound(input, report, stock);
        {
            // the WaW light grid, the sun, primary lights and their definitions
            const auto& lighting = input.lighting;
            const auto base = files::Utf8(lighting.base);
            const auto gridFile = files::FromUtf8(base + ".lightgrid.bin");
            if (fs::exists(gridFile))
            {
                const auto modelsFile = projectRoot / "BSP/models.json";
                world::GridRequest gridRequest{gridFile, projectRoot / "BSP/lightgrid.bin", std::nullopt, true};
                if (fs::exists(modelsFile))
                    gridRequest.models = {modelsFile, lighting.clipSource, files::FromUtf8(base + ".primarylights.json")};
                content["lighting"] = Timed("Bake static model lighting (WaW grid traces)", [&] { return world::StageGrid(gridRequest); });
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
                    const auto& dvars = lighting.scriptRenderDvars;
                    for (const auto& [k, v] : dvars.items())
                        for (const auto& [dvar, field] : sunDvars)
                            if (k == dvar)
                            {
                                double value = 0;
                                if (v.is_number())
                                    value = v.get<double>();
                                else if (!util::ParseFloat(v.get<std::string>(), value))
                                    throw StepError("ValueError", "could not convert string to float: " + util::StringRepr(v.get<std::string>()));
                                scriptValues[field] = value;
                            }
                    if (!scriptValues.empty())
                    {
                        auto staged = mod::Json(projectRoot / "BSP/lighting.json");
                        for (const auto& [k, v] : scriptValues.items())
                            staged["sun"][k] = v;
                        files::WriteText(projectRoot / "BSP/lighting.json", util::JsonDumps(staged, 2) + "\n");
                        content["lighting_script_sun"] = scriptValues;
                    }
                }
                const auto primaryFile = files::FromUtf8(base + ".primarylights.json");
                if (!fs::exists(primaryFile))
                    errors.push_back("source LightGrid requires its matching primary-light table");
                else
                {
                    const auto primary = world::StagePrimaryLights(primaryFile, projectRoot / "BSP/primarylights.json", lighting.shadowLights);
                    if (primary.contains("error"))
                        throw StepError("LightingError", primary["error"].get<std::string>());
                    if (!primary["unshadowed"].empty())
                        warnings.push_back(std::to_string(primary["unshadowed"].size()) +
                                           " primary spot/omni lights drawn without shadow maps (render dump has no shadow geometry: re-extract gfxworld with the v6 T4 dumper)");
                    std::set<std::string> definitions;
                    for (const auto& d : primary["definitions"])
                        definitions.insert(d.get<std::string>());
                    const auto lightdefs = StageLightDefs(projectRoot, roots, definitions);
                    for (const auto& i : lightdefs.images)
                        images.push_back(i);
                    for (const auto& e : lightdefs.errors)
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
            const auto timing = report.Script("window_repair_timing", Json());
            StageZbarrier(projectRoot, techsetDump, bo2Root, timing).MergeInto(report);
        }

        // stage_model_overlay_fx
        const auto overlays = effects::StageModelOverlayRuntime(projectRoot, report.fxTable);
        content["model_overlay_fx"] = effects::ToJson(overlays);
        std::vector<std::string> overlayVariants;
        for (const auto& entry : overlays)
            overlayVariants.push_back(entry.variant);
        // modelisolation.stage
        const auto modelRenames = models::IsolateModelNames(projectRoot);
        content["model_identities"] = models::RenamesJson(modelRenames);
        const models::ModelNames modelNames(modelRenames.begin(), modelRenames.end());
        // last script rewrite before the zone lists: T6 names live in a 64 KiB window per script
        if (const auto parts = SplitCoreScript(projectRoot); parts > 1)
            warnings.push_back("SCRIPT_CORE_SPLIT maps/mp/waw/_waw2bo2_core.gsc split into " + std::to_string(parts) +
                               " parts: T6 addresses a script's names with 16-bit offsets");
        const auto scripts = gsc::port::MapScripts(projectRoot, project);
        if (hasScripts && bo2Root)
        {
            // Validate the completed graph, including owned perk/bootstrap modules
            gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
            const auto api = gsc::port::T6Api::Load(extract::ResourceRoot() / "src/waw2bo2/compat/t6_script_api.json", stage / "t6api_cache.json");
            for (const auto& problem : gsc::port::LinkCheck(projectRoot, api))
                errors.push_back("scripts: link: " + problem);
        }
        const auto& rawRoots = input.rawRoots;
        bool menuReady = false;
        if (stock && hasScripts && wawMapStem)
        {
            // WaW's own pause menu (ui/hud.txt + ui/ingame.txt) replaces BO2's class menu
            std::error_code e;
            fs::remove(projectRoot / "content_source/waw_menu.json", e);
            const auto staged = Timed("Translate WaW in-game menus", [&] { return StageWawMenus(projectRoot, rawRoots, *wawMapStem); });
            if (staged.error)
                warnings.push_back("FRAMEWORK_BO2 pause menu: " + *staged.error + "; BO2 pause menu kept");
            else
            {
                content["waw_menu"] = staged.result;
                menuReady = true;
            }
        }
        // localization.stage: localize-only zone dumps live in the shared WaW cache
        const auto localizationDumps = stock ? stock->work / "localization_dumps" : stage / "localization_dumps";
        strings::LocalizationRequest localizationRequest{projectRoot, roots, input.iwdDirs, rawRoots, input.t4Unlinker, localizationDumps, bo2Root,
                                                         input.t6Unlinker, techsetDump, "english",
                                                         stock};
        const auto localization = Timed("Stage localization", [&] { return strings::StageLocalization(localizationRequest); });
        content["localization"] = localization;
        Json recovered = Json::object();
        if (menuReady)
        {
            std::map<std::string, std::string> localized;
            for (const auto& entry : localization.value("entries", Json::array()))
                localized[entry.at("key").get<std::string>()] = entry.at("value").get<std::string>();
            const auto art = Timed("Stage WaW menu art", [&] { return StageWawMenuArt(projectRoot, roots, stock, localized, recovery); });
            recovered.update(art.recovered);
            auto& menu = content["waw_menu"];
            menu["menus"] = art.menus;
            menu["materials"] = art.materials;
            menu["glyphs"] = art.glyphs;
            menu["missing"] = art.missing;
            for (const auto& name : art.missing)
                warnings.push_back("WAW_MENU_ASSET_MISSING " + name + ": drawn empty");
        }
        for (const auto& e : localization["errors"])
            errors.push_back(e);
        for (const auto& w : localization["warnings"])
            warnings.push_back(w);
        {
            std::string lines = "localize," + localization["asset"].get<std::string>() + "\n";
            if (input.bo2StockPerks)
            {
                lines += "// Opt-in stock BO2 perk dependencies\n";
                for (const auto& line : PerkAssetLines(projectRoot))
                    lines += line + "\n";
            }
            AppendZone(projectRoot, lines);
        }
        // modelcolours.stage_surface_fixes
        {
            const auto fixes = models::StageSurfaceFixes(projectRoot, SurfaceView(report.materials), true, true);
            content["additive_model_vertex_alpha"] = fixes.OpacityJson();
            if (!fixes.opacity.empty())
            {
                std::size_t vertices = 0;
                for (const auto& [_, n] : fixes.opacity)
                    vertices += n;
                warnings.push_back("ADDITIVE_MODEL_VERTEX_ALPHA " + std::to_string(vertices) + " vertices in " + std::to_string(fixes.opacity.size()) +
                                   " models: mc_unlit_add surfaces draw their vertex colour at alpha 1");
            }
            content["model_surface_sort_keys"] = fixes.SortKeysJson();
            if (!fixes.sortKeys.empty())
            {
                auto sorted = fixes.sortKeys;
                std::ranges::sort(sorted, {}, &models::SortKeyChange::material);
                std::string text;
                for (const auto& change : sorted)
                    text += (text.empty() ? "" : ", ") + change.material + " " + std::to_string(change.from) + "->" + std::to_string(change.to);
                warnings.push_back("MODEL_SURFACE_SORT_ORDER " + text + ": same WaW sort key, T6 key raised to keep the model surface order");
            }
        }
        // materialisolation.stage
        models::RenameList materialRenames;
        if (techsetDump)
        {
            std::vector<models::MaterialFile> entries;
            for (const auto& entry : report.materials)
                entries.push_back({entry.name, entry.file});
            const auto hud = report.Script("hud_materials").get<std::vector<std::string>>();
            auto result = models::IsolateMaterialNames(projectRoot, *techsetDump, entries, hud);
            for (const auto& update : result.updates)
            {
                auto& material = report.materials[update.index];
                material.name = update.name;
                material.file = update.file;
                material.sourceName = update.sourceName;
            }
            materialRenames = std::move(result.renamed);
            if (!materialRenames.empty())
                warnings.push_back("MATERIAL_IDENTITIES_ISOLATED " + std::to_string(materialRenames.size()) + " converted materials shared stock BO2 names");
        }
        content["material_identities"] = models::RenamesJson(materialRenames);
        const models::ModelNames materialNames(materialRenames.begin(), materialRenames.end());
        // constraints.stage_runtime
        const auto ropeRuntime = bridge::Reported([&] { return physics::StageRopeRuntime(projectRoot); });
        for (const auto& joint : ropeRuntime.value("unsupported_joints", Json::array()))
            warnings.push_back("MAP_JOINT_UNSUPPORTED " + joint.get<std::string>() + ": T6 has no map entity for it; the body hangs by its ropes only");
        content["physics_constraint_runtime"] = ropeRuntime;
        std::set<std::string> hudMaterials;
        for (const auto& n : input.hudMaterials)
            hudMaterials.insert(materialNames.contains(n) ? materialNames.find(n)->second : n);
        std::set<std::string> staged;
        for (const auto& m : report.materials)
            staged.insert(m.name);
        // rumbles.stage
        auto rumbleRoots = roots;
        rumbleRoots.insert(rumbleRoots.end(), rawRoots.begin(), rawRoots.end());
        const auto rumbles = StageRumbles(projectRoot, rumbleRoots, bo2Root ? *bo2Root / "raw" : fs::path());
        content["rumbles"] = rumbles.ToJson();
        for (const auto& name : rumbles.missing)
            errors.push_back("RUMBLE_DEPENDENCY_MISSING " + name);
        // write_zone
        {
            // sorted(model_names.get(m, m) for m in ...): two models may share an identity
            std::vector<std::string> xmodels;
            for (const auto& n : input.zoneModels)
                xmodels.push_back(modelNames.contains(n) ? modelNames.find(n)->second : n);
            std::sort(xmodels.begin(), xmodels.end());
            world::MapZoneAssets assets;
            assets.image = images;
            assets.xmodel = std::move(xmodels);
            assets.fx = input.zoneFx;
            assets.fx.insert(assets.fx.end(), overlayVariants.begin(), overlayVariants.end());
            for (const auto& m : hudMaterials)
                if (staged.count(m))
                    assets.material.push_back(m);
            for (const auto& m : ropeRuntime.value("materials", Json::array()))
                assets.material.push_back(m.get<std::string>());
            assets.script = scripts;
            assets.localize = {localization["asset"].get<std::string>()};
            assets.rawfile = rumbles.files;
            world::WriteMapZone(stage, project, input.ipak, assets);
        }
        // CLAUDE.md: every function BO2 runs in place of the map's own without a glue reason is reported
        const auto covered = FrameworkCoverage(project, projectRoot, input.coverageRoots, report.scripts);
        content["framework_coverage"] = covered.TotalsJson();
        for (const auto& w : covered.warnings)
            warnings.push_back(w);
        mod::Print("framework coverage: " + CountsRepr(covered.totals) + " (framework_coverage.md)");
        files::WriteText(projectRoot / "bridge_stage.report.json", util::JsonDumps(report.ToJson(), 2) + "\n");
        weaponRecovered.update(recovered);
        return weaponRecovered;
    }
} // namespace w2b::pipeline
