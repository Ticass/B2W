// The end of the bridge stage (see bridge_tail.h).
#include "bridge_tail.h"

#include "core/pyre.h"
#include "coverage_stage.h"
#include "effects/stage.h"
#include "extract/stockwaw.h"
#include "gsc/port/staging.h"
#include "hud_stage.h"
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

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::StartsWith;
    using mod::Str;
    using py::Json;

    namespace
    {
        // a native step's JsonRequest entry point, called in process
        Json Call(const std::function<Json(const fs::path&)>& step, const Json& request)
        {
            static int serial = 0;
            const auto folder = fs::temp_directory_path() / ("w2b_bridge_tail_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(serial++));
            fs::create_directories(folder);
            files::WriteText(folder / "request.json", py::JsonDumpsDefault(request));
            struct Cleanup
            {
                fs::path folder;
                ~Cleanup()
                {
                    std::error_code e;
                    fs::remove_all(folder, e);
                }
            } cleanup{folder};
            return step(folder / "request.json");
        }

        // progress.timed's lines
        template <typename F> auto Timed(const std::string& label, F&& f)
        {
            mod::Print("[staging] " + label + "...");
            const auto started = std::chrono::steady_clock::now();
            auto result = f();
            char text[64];
            std::snprintf(text, sizeof text, "%.1f", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            mod::Print("[staging] " + label + " finished in " + text + "s");
            return result;
        }

        Json Paths(const std::vector<fs::path>& paths)
        {
            Json out = Json::array();
            for (const auto& p : paths)
                out.push_back(Str(p));
            return out;
        }

        std::vector<fs::path> PathList(const Json& v)
        {
            std::vector<fs::path> out;
            if (v.is_array())
                for (const auto& p : v)
                    out.push_back(files::FromUtf8(p.get<std::string>()));
            return out;
        }

        std::optional<fs::path> OptionalPath(const Json& v)
        {
            return v.is_string() ? std::optional(files::FromUtf8(v.get<std::string>())) : std::nullopt;
        }

        Json PathOrNull(const std::optional<fs::path>& p)
        {
            return p ? Json(Str(*p)) : Json();
        }

        bool Truthy(const Json& v)
        {
            if (v.is_null())
                return false;
            if (v.is_object() || v.is_array() || v.is_string())
                return !v.empty();
            if (v.is_boolean())
                return v.get<bool>();
            if (v.is_number())
                return v.get<double>() != 0;
            return true;
        }

        void AppendZone(const fs::path& projectRoot, const std::string& lines)
        {
            std::string existing;
            files::ReadBytes(projectRoot / "mod_extra.zone", existing);
            files::WriteBytes(projectRoot / "mod_extra.zone", existing + mod::TextBytes(lines));
        }

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

    Json BridgeTail(const Json& request, textures::IwdRecovery& recovery)
    {
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
        return Json{{"report", report}, {"stock_prepared", stockJson.is_null() ? Json() : stockJson.value("prepared", Json())}, {"recovered", recovered}};
    }
} // namespace w2b::pipeline
