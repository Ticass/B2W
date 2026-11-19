// cli build-mod: mod.ff, its bound shaders and the lobby zone (see modzone.h).
#include "modzone.h"

#include "core/pyre.h"
#include "core/timeline.h"
#include "gsc/port/generators.h"
#include "shaders/techsets.h"
#include "hud.h"
#include "modsupport.h"
#include "modutil.h"
#include "shaderbind.h"

#include <map>

namespace w2b::pipeline
{
    using namespace mod;

    namespace
    {
        // (root, report entry) of every material THIS build staged; the stage
        // folder is reused across builds, so files on disk alone may be stale
        std::vector<std::pair<fs::path, py::Json>> StagedMaterials(const fs::path& project)
        {
            std::vector<std::pair<fs::path, py::Json>> entries;
            if (fs::exists(project / "bridge_stage.report.json"))
                for (const auto& e : Member(project / "bridge_stage.report.json", "materials"))
                    entries.emplace_back(project, e);
            if (fs::exists(project / "content_source/weapons.visuals.json"))
                for (const auto& e : Member(project / "content_source/weapons.visuals.json", "materials"))
                    entries.emplace_back(project / "content_source", e);
            return entries;
        }

        // baseline_shader_materials: the raw mod-tools linker gets its stock
        // techniques; staging is restored afterwards
        class BaselineShaderMaterials
        {
        public:
            BaselineShaderMaterials(const fs::path& project, const fs::path& stock)
            {
                try
                {
                    for (const auto& [root, entry] : StagedMaterials(project))
                    {
                        const auto runtime = entry.value("shader_runtime", py::Json::object());
                        if (!Truthy(runtime, "active") && !Truthy(runtime, "native_probe_passes"))
                            continue;
                        const auto path = root / files::FromUtf8(entry["file"].get<std::string>());
                        const auto key = Lower(Str(Resolve(path)));
                        if (!IsFile(path) || originals.count(key))
                            continue;
                        auto native = runtime.value("native_techset", py::Json());
                        if (!native.is_string() || native.get<std::string>().empty())
                            native = Json(stock / "materials" / files::FromUtf8(entry["donor"].get<std::string>()))["techniqueSet"];
                        const auto bytes = Bytes(path);
                        originals[key] = {path, bytes};
                        auto material = py::Json::parse(bytes);
                        material["techniqueSet"] = native;
                        files::WriteText(path, py::JsonDumpsDefault(material));
                    }
                }
                catch (...)
                {
                    Restore();
                    throw;
                }
            }
            ~BaselineShaderMaterials()
            {
                Restore();
            }
            bool Active() const
            {
                return !originals.empty();
            }

        private:
            void Restore()
            {
                for (const auto& [_, original] : originals)
                    files::WriteBytes(original.first, original.second);
                originals.clear();
            }
            std::map<std::string, std::pair<fs::path, std::string>> originals;
        };

        std::set<std::string> TextureSlots(const py::Json& material)
        {
            std::set<std::string> slots;
            for (const auto& t : material.value("textures", py::Json::array()))
                slots.insert(t.contains("name") && t["name"].is_string() ? t["name"].get<std::string>() : std::string("\x01None"));
            return slots;
        }

        // a leading comma requests an already loaded image; the extracted asset reports its resolved name
        py::Json TextureRoutes(const py::Json& material)
        {
            auto routes = py::Json::array();
            for (auto t : material.value("textures", py::Json::array()))
            {
                auto image = t.value("image", std::string());
                if (StartsWith(image, ","))
                    image.erase(0, 1);
                t["image"] = image;
                routes.push_back(py::SortKeys(t));
            }
            return routes;
        }

        void RemoveInside(const fs::path& work, const fs::path& directory)
        {
            if (Lower(Str(Resolve(directory))).rfind(Lower(Str(work)), 0) != 0)
                throw StepError("ValueError", "'" + Str(directory) + "' is not in the subpath of '" + Str(work) + "'");
            std::error_code e;
            fs::remove_all(directory, e);
        }
    } // namespace

    void ActivateModShaders(const fs::path& ff, const fs::path& project, const fs::path& workIn, const fs::path& linker, const fs::path& unlinker, const fs::path& stock)
    {
        // relink the fresh gameplay zone with bound shaders over the complete
        // baseline: bound materials, shader dependencies and flame-table
        // weapons are fresh inputs, every other asset keeps its baseline data
        const auto work = Resolve(workIn);
        for (const char* relative : {"shader_baseline/dump", "shader_overlay", "shader_check", "shader_out"})
            RemoveInside(work, work / relative);
        const auto baseline = work / "shader_baseline/mod.ff";
        MakeParent(baseline);
        Copy(ff, baseline);
        const auto dump = work / "shader_baseline/dump";
        auto proc = Captured({Str(Resolve(unlinker)), "--no-color", "--include-assets", "material", "--output-folder", Str(dump), Str(baseline)});
        files::WriteText(work / "shader_baseline/unlinker.log", Text(proc.out));
        if (proc.exitCode)
            throw StepError("RuntimeError", "shader baseline material extraction failed");
        const auto overlay = work / "shader_overlay";
        // the mod-tools linker leaves nested flame-table pointers empty: those
        // weapons are relinked by the native loader
        const auto content = project / "content_source";
        if (IsFile(content / "weapons.stage.json"))
        {
            const auto data = Json(content / "weapons.stage.json");
            for (const auto& entry : data.value("flame_tables", py::Json::array()))
            {
                const auto file = files::FromUtf8(entry["file"].get<std::string>());
                MakeParent(overlay / file);
                Copy(content / file, overlay / file);
            }
            for (const auto& entry : data.value("weapons", py::Json::array()))
            {
                const auto deps = entry.value("dependencies", py::Json::object());
                if (!Truthy(deps, "flametable"))
                    continue;
                const auto name = files::FromUtf8(entry["name"].get<std::string>());
                MakeParent(overlay / "weapons" / name);
                Copy(content / "weapons" / name, overlay / "weapons" / name);
            }
        }
        std::set<std::string> bound;
        std::set<std::string> currentFiles;
        std::optional<timeline::Span> phase(std::in_place, "bind runtime shader materials");
        for (const auto& [root, entry] : StagedMaterials(project))
            currentFiles.insert(Lower(Str(Resolve(root / files::FromUtf8(entry["file"].get<std::string>())))));
        for (const auto& root : {project, project / "content_source"})
            for (const auto& source : files::RGlob(root / "materials", ".json"))
            {
                if (!currentFiles.count(Lower(Str(Resolve(source)))))
                    continue; // left by an earlier build
                const auto current = Json(source);
                if (!StartsWith(current.value("techniqueSet", std::string()), "waw/runtime_"))
                    continue;
                const auto relative = source.lexically_relative(root);
                const auto extracted = dump / relative;
                if (!IsFile(extracted))
                    continue; // a world-zone material, not in mod.ff
                auto material = Json(extracted);
                const auto have = TextureSlots(material);
                const auto want = TextureSlots(current);
                // same name, different asset: mod.ff holds BO2's own material
                // without the WaW slots the runtime technique reads
                if (!std::includes(have.begin(), have.end(), want.begin(), want.end()))
                    continue;
                material["techniqueSet"] = current["techniqueSet"];
                MakeParent(overlay / relative);
                files::WriteText(overlay / relative, py::JsonDumpsDefault(material));
                bound.insert(Generic(relative));
            }
        phase.reset();
        {
            timeline::Span span("copy shader overlay");
            for (const auto& root : {stock, project, project / "content_source"})
                for (const char* folder : {"techniquesets", "shader_bin", "english/localizedstrings"})
                    if (IsDir(root / folder))
                        CopyTree(root / folder, overlay / folder);
        }
        phase.emplace("bind impact materials");
        const auto impact = BindImpactMaterials(project, dump, overlay, stock);
        phase.emplace("bind probe materials");
        for (const auto& [relative, _] : impact)
            bound.insert(relative);
        std::map<std::string, int> probes;
        if (IsFile(project / "BSP/reflectionprobes.json"))
        {
            // the restored source cube channels also reach stock gameplay materials
            probes = BindNativeProbeMaterials(dump, overlay, stock);
            py::Json report = py::Json::object();
            for (const auto& [relative, count] : probes)
            {
                bound.insert(relative);
                report[relative] = count;
            }
            files::WriteText(work / "native_probe_materials.json", py::JsonDumps(report, 1));
        }
        phase.reset();
        // stringtables and project rawfiles override loaded and stock assets
        for (const char* name : {"mapstable.csv", "gametypestable.csv", "zm_statstable.csv", "waw_menu_layers.csv", "waw_menu_preview.csv", "waw_hud_maps.csv",
                                 "waw_hud_weapons.csv", "waw_hud_glyphs.csv"})
            if (IsFile(project / "zm" / name))
            {
                MakeParent(overlay / "zm" / name);
                Copy(project / "zm" / name, overlay / "zm" / name);
            }
        for (const char* folder : {"ui_mp", "ui"})
            for (const auto& source : files::RGlob(project / folder, ".lua"))
            {
                const auto target = overlay / source.lexically_relative(project);
                MakeParent(target);
                Copy(source, target);
            }
        auto zone = Read(work / "zone_source/mod.zone");
        // explicit entries make the linker replace baseline models' material pointers
        std::set<std::string> rebound;
        for (const auto& [relative, _] : probes)
            rebound.insert(relative);
        for (const auto& [relative, _] : impact)
            rebound.insert(relative);
        for (auto name : rebound)
        {
            if (StartsWith(name, "materials/"))
                name.erase(0, 10);
            if (EndsWith(name, ".json"))
                name.resize(name.size() - 5);
            zone += "\nmaterial," + name + "\n";
        }
        if (!impact.empty())
        {
            // source textures go to a separate pack beside the baseline's
            const auto pack = files::Utf8(project.filename()) + "_impact_materials";
            zone = pyre::Regex("(?m)^>ipak,.*$").Sub([&](const pyre::Match&) { return ">level.ipak_read," + pack + "\n>ipak," + pack; }, zone);
        }
        // raw weapon files live under mp/sp; loaded assets use engine-internal names
        zone = pyre::Regex("(?m)^weapon,(?:mp|sp)/").Sub("weapon,", zone);
        // the template's singleton "fximpacttable,,ImpactFx" would reference the
        // empty-named stock table by a name the game cannot load
        zone = pyre::Regex("(?m)^fximpacttable,.*(?:\\r?\\n|$)").Sub("", zone);
        const auto sourceRoot = work / "shader_zone_source";
        fs::create_directories(sourceRoot);
        files::WriteText(sourceRoot / "mod.zone", zone);
        const auto output = work / "shader_out";
        proc = Captured({Str(Resolve(linker)), "--no-color", "--base-folder", Str(work), "--load", Str(baseline), "--source-search-path", Str(sourceRoot),
                         "--asset-search-path", Str(overlay), "--output-folder", Str(output), "mod"});
        const auto log = Text(proc.out);
        files::WriteText(work / "shader_linker.log", log);
        const auto linked = output / "mod.ff";
        if (proc.exitCode || !IsFile(linked) || pyre::Regex("(?m)^ERROR:").Search(log))
            throw StepError("RuntimeError", "bound shader mod relink failed (" + std::to_string(proc.exitCode) + "); see " + Str(work / "shader_linker.log"));
        const auto check = work / "shader_check";
        proc = Captured({Str(Resolve(unlinker)), "--no-color", "--include-assets", "material,techniqueset", "--output-folder", Str(check), Str(linked)});
        files::WriteText(work / "shader_check.log", Text(proc.out));
        if (proc.exitCode)
            throw StepError("RuntimeError", "bound shader mod roundtrip failed");
        for (const auto& relative : bound)
        {
            const auto expected = Json(overlay / files::FromUtf8(relative));
            const auto actual = Json(check / files::FromUtf8(relative));
            if (actual["techniqueSet"] != expected["techniqueSet"] || TextureRoutes(actual) != TextureRoutes(expected))
                throw StepError("RuntimeError", "bound material or texture routing changed: " + relative);
            const auto technique = shaders::ReadT6TechniqueSet(check / "techniquesets" / files::FromUtf8(actual["techniqueSet"].get<std::string>() + ".json"));
            for (const auto& slot : technique.techniques)
                if (slot)
                    for (const auto& shaderPass : slot.technique->passes)
                        for (const auto& [name, stage] : {std::pair<const std::string&, const char*>{shaderPass.pixelShader, "ps"}, {shaderPass.vertexShader, "vs"}})
                            if (StartsWith(name, "waw/runtime_") && Bytes(check / "shader_bin" / files::FromUtf8(std::string(stage) + "_" + name + ".cso")).substr(0, 4) != "DXBC")
                                throw StepError("RuntimeError", "linked translated shader is not DXBC: " + name);
        }
        Copy(linked, ff);
        if (!impact.empty())
        {
            std::error_code e;
            for (const auto& entry : fs::directory_iterator(output, e))
                if (EndsWith(Lower(files::Utf8(entry.path().filename())), "_impact_materials.ipak"))
                {
                    RepairIpak(entry.path());
                    Copy(entry.path(), ff.parent_path() / entry.path().filename());
                }
        }
        files::WriteText(work / "shader_activation.json",
                         py::JsonDumps(py::Json{{"materials", std::vector<std::string>(bound.begin(), bound.end())}, {"baseline", Str(baseline)}, {"output", Str(ff)}, {"preserved_ipak", true}}, 2));
    }

    namespace
    {
        // _finish_map_scripts: the map scripts lose references to weapons
        // mod.ff lacks; final once the first mod.ff link is done
        void FinishMapScripts(const BuildModOptions& o, const std::vector<std::string>& unavailable)
        {
            const auto weapons = UnavailableWeapons(unavailable);
            const auto projectRoot = Resolve(o.stage) / "zone_raw" / files::FromUtf8(o.project);
            for (const auto& entry : unavailable)
                Print("template entry unavailable: " + entry);
            for (const auto& script : MapScripts(projectRoot, o.project))
                for (const auto& line : PruneWeaponReferences(projectRoot / files::FromUtf8(script), weapons))
                    Print(script + ": disabled " + line);
            // a weapon no zone carries makes T6 abort on "unknown weapon": the
            // compat lookup skips names outside this list
            auto known = LoadedWeapons(o.work);
            if (!known.empty())
            {
                const auto cache = Resolve(o.stage) / "t6api_cache.json";
                if (IsFile(cache))
                {
                    const auto data = Json(cache);
                    if (data.contains("stock_assets") && data["stock_assets"].contains("weapon"))
                        for (const auto& w : data["stock_assets"]["weapon"])
                            known.insert(w.get<std::string>());
                }
                known.insert("none");
                const auto target = projectRoot / "maps" / "mp" / "waw" / "_waw2bo2_bo2weapons.gsc";
                MakeParent(target);
                files::WriteText(target, gsc::port::Bo2WeaponsSource(known));
                Print("BO2 weapons loaded with the map: " + std::to_string(known.size()) + " (unknown script weapons are skipped at runtime)");
            }
            else
                Print("warning: no weapons recorded in the mod.ff linker log; script weapon names are not checked");
            if (o.scriptsReady)
            {
                MakeParent(*o.scriptsReady);
                files::WriteText(*o.scriptsReady, "ready\n");
            }
        }
    } // namespace

    void BuildMod(const BuildModOptions& o)
    {
        const auto bo2 = Resolve(o.bo2), unlinker = Resolve(o.unlinker);
        const auto projectRoot = Resolve(o.stage) / "zone_raw" / files::FromUtf8(o.project);
        const auto extra = projectRoot / "mod_extra.zone";
        std::vector<std::string> extraLines = fs::exists(extra) ? Lines(Read(extra)) : std::vector<std::string>{};
        const auto add = [&](const std::vector<std::string>& lines)
        {
            for (const auto& entry : lines)
                if (std::find(extraLines.begin(), extraLines.end(), entry) == extraLines.end())
                    extraLines.push_back(entry);
        };
        add(StageOverheatHud(bo2, projectRoot, o.work, unlinker));
        add(StageHudRuntime(bo2, projectRoot, o.project));
        StageLobbyMapTable(bo2 / "raw/zm/mapstable.csv", projectRoot, o.project);
        StageLobbyGametypeTable(bo2 / "raw/zm/gametypestable.csv", projectRoot, o.project);
        add(StageMenuAssets(projectRoot, o.project));
        add({"stringtable,zm/mapstable.csv", "stringtable,zm/gametypestable.csv"});
        fs::path ff;
        std::vector<std::string> unavailable;
        std::vector<fs::path> roots{projectRoot / "content_source/pcm", projectRoot / "content_source"};
        if (o.linker && o.techsetDump)
        {
            bool active;
            {
                BaselineShaderMaterials baseline(projectRoot, *o.techsetDump);
                active = baseline.Active();
                timeline::Span span("link baseline mod.ff");
                roots.push_back(*o.techsetDump);
                std::tie(ff, unavailable) = LinkMod(bo2, o.work, unlinker, extraLines, projectRoot, roots, std::nullopt, o.techsetDump);
            }
            FinishMapScripts(o, unavailable);
            const auto flameStage = projectRoot / "content_source/weapons.stage.json";
            const bool flames = IsFile(flameStage) && [&]
            {
                const auto data = Json(flameStage);
                return data.contains("flame_tables") && !data["flame_tables"].empty() && !data["flame_tables"].is_null();
            }();
            if (active || flames)
            {
                timeline::Span span("activate mod shaders");
                ActivateModShaders(ff, projectRoot, o.work, *o.linker, o.unlinker, *o.techsetDump);
            }
        }
        else
        {
            std::tie(ff, unavailable) = LinkMod(bo2, o.work, unlinker, extraLines, projectRoot, roots, std::nullopt, o.techsetDump);
            FinishMapScripts(o, unavailable);
        }
        Print("mod.ff: " + Str(ff));
        std::vector<fs::path> packs;
        std::error_code e;
        for (const auto& entry : fs::directory_iterator(ff.parent_path(), e))
            if (EndsWith(Lower(files::Utf8(entry.path().filename())), ".ipak"))
                packs.push_back(entry.path());
        std::sort(packs.begin(), packs.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
        for (const auto& pack : packs)
        {
            const auto moved = RepairIpak(pack);
            if (!moved.empty())
                Print("warning: IPAK_WINDOW_REPAIRED " + files::Utf8(pack.filename()) + ": " + std::to_string(moved.size()) +
                      " block(s) moved to a streaming window start (the BO2 mod tools linker let them cross one; t6zm overruns its buffer there)");
        }
        const auto lobby = LinkLobby(bo2, projectRoot, o.project, o.work, o.linker ? *o.linker : bo2 / "bin/Linker.exe", o.techsetDump);
        for (const auto& zone : {ff, lobby})
            VerifyLobbyTables(zone, o.unlinker, o.work, o.project);
        Print("mod_load.ff: " + Str(lobby));
    }
} // namespace w2b::pipeline
