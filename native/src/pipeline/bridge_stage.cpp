// The bridge stage (see bridge_stage.h): the port of t6bridge.stage_bridge.
// Each pass keeps its Python order, report entries and progress lines; the
// passes themselves are the native steps, called in this process.
#include "bridge_stage.h"

#include "actor_trees.h"
#include "audio/flac_batch.h"
#include "bridge_assets.h"
#include "bridge_geometry.h"
#include "bridge_support.h"
#include "bridge_tail.h"
#include "core/pyre.h"
#include "core/zip.h"
#include "materials/t6_material.h"
#include "core/timeline.h"
#include "effects_stage.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "extract/wawsource.h"
#include "finish.h"
#include "gsc/port/api.h"
#include "gsc/port/generators.h"
#include "mapents/entities.h"
#include "models/models.h"
#include "models/projectile_collision.h"
#include "physics/physics.h"
#include "scripts_stage.h"
#include "skybox_stage.h"
#include "textures/iwd_recovery.h"
#include "textures/stage.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using bridge::AppendZone;
    using bridge::Extend;
    using bridge::Names;
    using bridge::PathOrNull;
    using bridge::Paths;
    using bridge::Timed;
    using bridge::Truthy;
    using mod::Str;
    using py::Json;

    namespace
    {
        constexpr const char* SKY_SOURCE_DIR = "sky_src";
        constexpr const char* FX_OUTPUT_PREFIX = "waw/";

        // ------------------------------------------------------------ arguments

        fs::path Resolved(const std::string& value)
        {
            return extract::Resolve(files::FromUtf8(value));
        }

        // ------------------------------------------------------------ small helpers

        std::vector<fs::path> Concat(std::initializer_list<std::vector<fs::path>> parts)
        {
            std::vector<fs::path> out;
            for (const auto& part : parts)
                out.insert(out.end(), part.begin(), part.end());
            return out;
        }

        std::vector<fs::path> Optional(const std::optional<fs::path>& path)
        {
            return path ? std::vector<fs::path>{*path} : std::vector<fs::path>{};
        }

        std::vector<fs::path> ImageFolders(const std::vector<fs::path>& roots)
        {
            std::vector<fs::path> folders;
            for (const auto& root : roots)
                folders.push_back(root / "images");
            return folders;
        }

        Names NameSet(const Json& list)
        {
            Names names;
            if (list.is_array())
                for (const auto& name : list)
                    names.insert(name.get<std::string>());
            return names;
        }

        Json SortedNames(const Names& names)
        {
            return Json(std::vector<std::string>(names.begin(), names.end()));
        }

        Names Union(Names left, const Names& right)
        {
            left.insert(right.begin(), right.end());
            return left;
        }

        Names Difference(Names left, const Names& right)
        {
            for (const auto& name : right)
                left.erase(name);
            return left;
        }

        Names Intersection(const Names& left, const Names& right)
        {
            Names out;
            std::ranges::set_intersection(left, right, std::inserter(out, out.end()));
            return out;
        }

        // a report value a script pass recorded, or the fallback when the map has no scripts
        Json Script(const Json& report, const char* key, Json fallback = Json::array())
        {
            const auto& scripts = report["scripts"];
            return scripts.is_object() && scripts.contains(key) ? scripts[key] : fallback;
        }

        std::string StemWithout(const fs::path& file, std::string_view suffix)
        {
            auto name = files::Utf8(file.filename());
            if (name.ends_with(suffix))
                name.resize(name.size() - suffix.size());
            return name;
        }

        fs::path Sibling(const fs::path& file, std::string_view suffix, const std::string& extension)
        {
            return file.parent_path() / files::FromUtf8(StemWithout(file, suffix) + extension);
        }

        // native_tree_xanims: the animation names of a native T6 animtree (leaves, not groups)
        std::vector<std::string> NativeTreeXanims(const std::string& atr)
        {
            static const pyre::Regex comment("//[^\\n]*");
            static const pyre::Regex word("[A-Za-z0-9_]+");
            static const pyre::Regex group("^\\s*([A-Za-z0-9_]+)\\s*\\n\\s*\\{", pyre::MULTILINE);
            const auto text = comment.Sub("", atr);
            std::vector<std::string> names;
            for (const auto line : py::SplitLines(text))
            {
                const std::string stripped(py::Strip(line));
                if (word.FullMatch(stripped) && std::ranges::find(names, stripped) == names.end())
                    names.push_back(stripped);
            }
            Names groups;
            for (const auto& match : group.FindIter(text))
                groups.insert(match[1]);
            std::erase_if(names, [&](const std::string& name) { return groups.contains(name); });
            return names;
        }

        // the map's worldspawn skyboxmodel
        std::optional<std::string> SourceSkyModel(const fs::path& entities)
        {
            if (!mod::IsFile(entities))
                return std::nullopt;
            for (const auto& entity : mapents::Parse(mod::Read(entities)))
                if (entity.Get("classname") == "worldspawn")
                    return entity.Has("skyboxmodel") ? std::optional(std::string(entity.Get("skyboxmodel"))) : std::nullopt;
            return std::nullopt;
        }

        // entities.placed_models / strip_models / network_client_models over BSP/entities.json
        Names PlacedModels(const fs::path& entitiesJson)
        {
            Names names;
            for (const auto& entity : mapents::ReadMapEntities(entitiesJson))
                if (const auto model = entity.Get("model"); !model.empty() && !model.starts_with('*'))
                    names.insert(std::string(model));
            return names;
        }

        std::size_t StripModels(const fs::path& entitiesJson, const Names& names)
        {
            auto entities = mapents::ReadMapEntities(entitiesJson);
            std::size_t count = 0;
            for (auto& entity : entities)
                if (const auto* model = entity.Find("model"); model && names.contains(*model))
                {
                    entity.Erase("model");
                    ++count;
                }
            mapents::WriteMapEntities(entitiesJson, entities);
            return count;
        }

        // client-side script_models (spawnflag 2) whose targetname a WaW client
        // listener drives become server entities: the port runs those listeners on the server
        std::vector<std::string> NetworkClientModels(const fs::path& entitiesJson, const Names& listened)
        {
            constexpr long long CLIENT_SIDE_MODEL = 2;
            auto entities = mapents::ReadMapEntities(entitiesJson);
            std::map<std::string, int> changed;
            for (auto& entity : entities)
            {
                long long flags = 0;
                bool overflow = false;
                const auto text = entity.Get("spawnflags", "0");
                if (!text.empty())
                    py::ParseInt(text, flags, overflow);
                const auto target = std::string(entity.Get("targetname"));
                if (entity.Get("classname") == "script_model" && (flags & CLIENT_SIDE_MODEL) && entity.Has("targetname") && listened.contains(target))
                {
                    entity.Set("spawnflags", std::to_string(flags & ~CLIENT_SIDE_MODEL));
                    ++changed[target];
                }
            }
            if (!changed.empty())
                mapents::WriteMapEntities(entitiesJson, entities);
            std::vector<std::string> lines;
            for (const auto& [name, count] : changed)
                lines.push_back(name + " (" + std::to_string(count) + ")");
            return lines;
        }

        // ------------------------------------------------------------ the stage

        // what the effects pass hands on (EffectsResult)
        struct Effects
        {
            std::map<std::string, std::string> table; // script-loaded WaW effect -> converted name
            std::vector<std::string> zoneFx;
            Names models, sounds, techsets;
            std::vector<fs::path> extraRoots;
        };

        class BridgeStage
        {
        public:
            explicit BridgeStage(const BridgeStageOptions& options) : o_(options) {}

            const BridgeReport& Run();

        private:
            void OpenSources();
            void StageIwdWeapons(const std::vector<fs::path>& mapIwdDirs);
            void StageGeometry();
            void OpenModToolsSources();
            void PortScripts();
            void StageEffects(const std::vector<std::string>& names);
            void WriteEffectTable(const std::vector<std::string>& names);
            void AddAnimationZones();
            void StageMapModels();
            void StageDynents();
            Names RecoverProjectileModels();
            Names StageEffectModels();
            struct StagedMaterials
            {
                Json hud;   // the HUD materials the scripts draw
                Names used; // technique sets
            };
            StagedMaterials StageAllMaterials();
            void StageLightmaps(const Names& wawMaterials);
            std::vector<std::string> StageSkyAndImages(Names& used);
            void Tail(TailScripts scripts, std::vector<std::string> images, const Json& hudMaterials, const Names& zoneModels);

            std::vector<fs::path> MapScriptFolder() const
            {
                return o_.wawMapScript ? std::vector<fs::path>{o_.wawMapScript->parent_path().parent_path()} : std::vector<fs::path>{};
            }

            const BridgeStageOptions& o_;
            fs::path projectRoot_;
            BridgeReport report_;
            std::vector<fs::path> roots_, imageIwds_;
            std::optional<extract::StockWaw> stock_;
            std::optional<extract::WawSource> source_; // --waw-source-fx: WaW Mod Tools sources
            std::unique_ptr<textures::IwdRecovery> recovery_;
            Json recovered_ = Json::object(); // IWD images decoded so far (wavelet.recovered)

            // the geometry stage's results
            std::vector<GeometrySurface> surfaces_;
            std::vector<world::WorldStaticModel> staticModels_;
            Json mapDynents_;
            bool shadowLights_ = false, hasWater_ = false, layeredVertexData_ = false;
            std::optional<std::string> skyImage_;
            std::size_t probeCount_ = 0;
            Names clipModels_;

            // the navigation stage's results
            Names compiledWeapons_, zoneSounds_, shipped_, scriptModels_, entityBoxModels_, machineModels_, compiledSounds_;
            fs::path entitiesSource_;
            std::optional<gsc::port::ZoneGraph> zoneGraph_;
            std::map<std::string, std::string> dogStates_;

            std::optional<Json> weaponReport_;
            Effects effects_;
            Names resolved_, modelMaterials_;
            std::map<std::string, fs::path> nativeFallbacks_;
        };

        void BridgeStage::OpenSources()
        {
            projectRoot_ = o_.stage / "zone_raw" / files::FromUtf8(o_.project);
            fs::create_directories(projectRoot_);
            report_ = BridgeReport{};
            report_.project = o_.project;
            roots_ = Concat({{o_.stage}, o_.extraRoots});
            if (o_.wawRoot && o_.t4Unlinker)
            {
                stock_.emplace(*o_.wawRoot, *o_.t4Unlinker, o_.wawStockDumps.value_or(o_.stage / "waw_stock_dumps"));
                stock_->Load();
            }
            imageIwds_ = o_.iwdDirs.value_or(std::vector<fs::path>{});
            std::optional<fs::path> stockIwds;
            if (o_.wawRoot)
            {
                imageIwds_.push_back(*o_.wawRoot / "main");
                stockIwds = extract::Resolve(*o_.wawRoot / "main");
            }
            // the IWD images the map's own archives override come from them, not the stock ones
            std::vector<fs::path> sourceImages{projectRoot_ / SKY_SOURCE_DIR}, overrides;
            const auto stockScripts = o_.wawStockScripts ? std::optional(extract::Resolve(*o_.wawStockScripts)) : std::nullopt;
            for (const auto& root : roots_)
                if (!stockScripts || extract::Resolve(root) != *stockScripts)
                    sourceImages.push_back(root / "images");
            for (const auto& folder : imageIwds_)
                if (extract::Resolve(folder) != stockIwds)
                    overrides.push_back(folder);
            // the WaW Mod Tools' AssetViewer decodes wavelet IWIs (wawsource.locate_mod_tools)
            std::optional<fs::path> modTools = o_.wawModTools;
            if (!modTools && o_.wawRoot && mod::IsFile(*o_.wawRoot / "bin/linker_pc.exe"))
                modTools = o_.wawRoot;
            else if (!modTools && mod::IsFile(extract::ResourceRoot() / "wawModTools/bin/linker_pc.exe"))
                modTools = extract::ResourceRoot() / "wawModTools";
            const auto wavelet = modTools ? std::optional(*modTools / "bin/AssetViewer.exe") : std::nullopt;
            recovery_ = std::make_unique<textures::IwdRecovery>(imageIwds_, projectRoot_ / "wavelet_src", wavelet, sourceImages, overrides);
            StageIwdWeapons(overrides);
        }

        // A weapon file in the map's own IWDs (weapons/sp/<name>) is the one WaW
        // runs, over the copy linked into the map's zones: Clinic of Evil links
        // the stock Colt as zombie_colt and ships the Enforcer under that name in
        // sanatorium.iwd, and players spawn with the Enforcer. The files go to a
        // root ahead of the zone dumps (weapon sources resolve first root first);
        // within a folder the later archive wins, as WaW searches it first.
        void BridgeStage::StageIwdWeapons(const std::vector<fs::path>& mapIwdDirs)
        {
            const auto root = projectRoot_ / "iwd_weapons";
            std::error_code e;
            fs::remove_all(root, e);
            std::set<std::string> seen;
            std::vector<std::string> overridden;
            for (const auto& directory : mapIwdDirs)
            {
                if (!fs::is_directory(directory, e))
                    continue;
                std::vector<fs::path> archives;
                for (const auto& entry : fs::directory_iterator(directory, e))
                    if (mod::Lower(files::Utf8(entry.path().filename())).ends_with(".iwd"))
                        archives.push_back(entry.path());
                std::ranges::stable_sort(archives, std::ranges::greater{}, [](const fs::path& path) { return mod::Lower(files::Utf8(path.filename())); });
                for (const auto& path : archives)
                {
                    zip::Archive archive(path);
                    for (const auto& entry : archive.Entries())
                    {
                        const auto name = mod::Lower(entry.name);
                        if (entry.IsDir() || !name.starts_with("weapons/sp/") || name.find('/', 11) != std::string::npos)
                            continue;
                        const auto weapon = name.substr(11);
                        if (weapon.empty() || !seen.insert(weapon).second)
                            continue;
                        auto bytes = archive.Read(entry);
                        if (!bytes.starts_with("WEAPONFILE"))
                            continue;
                        files::WriteBytes(root / "weapons" / files::FromUtf8(weapon), bytes);
                        overridden.push_back(weapon);
                    }
                }
            }
            if (overridden.empty())
                return;
            roots_.insert(roots_.begin(), root);
            std::string names;
            for (const auto& weapon : overridden)
                names += (names.empty() ? "" : ", ") + weapon;
            report_.Warn("IWD_WEAPONS " + std::to_string(overridden.size()) + " weapon files from the map's IWDs take precedence over its zones: " + names);
        }

        void BridgeStage::StageGeometry()
        {
            const auto traversalRoots = Concat({o_.wawPatchScriptRoots, MapScriptFolder(), o_.wawScriptRoots, roots_, Optional(o_.wawStockScripts)});
            Json levelWeapons = Json::array();
            if (stock_ && stock_->Index().contains("common") && stock_->Index()["common"].contains("weapon"))
                levelWeapons = stock_->Index()["common"]["weapon"];
            NavigationRequest navigation{o_.project, o_.stage, o_.bo2, o_.wawMapScript, traversalRoots, o_.mapRoots, levelWeapons, o_.bo2StockPerks,
                                         gsc::port::Api()["zombie_runtime"]["source_respawn"]["native_exclusion_tag"].get<std::string>()};
            const GeometryRequest request{o_.gfx, o_.clip, projectRoot_, roots_, ImageFolders(roots_), stock_ ? &*stock_ : nullptr, true,
                                          Sibling(o_.gfx, ".gfx.bin", ".reflectionprobes.json"), Sibling(o_.gfx, ".gfx.bin", ".visibility.json"),
                                          Sibling(o_.clip, ".clip.bin", ".dynents.json"), Sibling(o_.clip, ".clip.bin", ".constraints.json"),
                                          std::move(navigation)};
            auto staged = Timed("Convert world geometry", [&] { return StageBridgeGeometry(request); });
            const auto& probes = staged.probes;
            Extend(report_, "images", probes["images"]);
            Extend(report_, "warnings", probes["warnings"]);
            Extend(report_, "errors", probes["errors"]);
            auto& content = report_.content;
            if (probes.contains("content"))
                content["reflection_probes"] = probes["content"];
            const auto warningIndex = report_.warnings.size();
            content["visibility"] = nullptr;
            content["physics_constraints"] = staged.constraints;
            layeredVertexData_ = staged.layered;
            for (const auto& [key, value] : staged.content.items())
                content[key] = value;
            if (content["visibility"].is_null())
                content.erase("visibility");
            report_.collision = std::move(staged.collision);
            for (const auto& warning : staged.warnings)
                report_.Warn(warning);
            if (staged.visibilityWarning)
                report_.warnings.insert(report_.warnings.begin() + static_cast<std::ptrdiff_t>(warningIndex), *staged.visibilityWarning);
            for (const auto& error : staged.errors)
                report_.Fail(error);
            surfaces_ = std::move(staged.surfaces);
            staticModels_ = std::move(staged.staticModels);
            shadowLights_ = staged.shadowLights;
            skyImage_ = staged.sky;
            probeCount_ = probes.contains("kept") ? probes["kept"].size() : 0;
            mapDynents_ = std::move(staged.dynents);
            clipModels_ = {staged.clipModels.begin(), staged.clipModels.end()};
            hasWater_ = staged.hasWater;
            auto& nav = staged.navigation.value(); // the bridge stage always asks for navigation
            compiledWeapons_ = std::move(nav.inventory.compiledWeapons);
            zoneSounds_ = std::move(nav.inventory.zoneSounds);
            shipped_ = std::move(nav.inventory.shippedModels);
            entitiesSource_ = nav.entitySource;
            report_.entities = std::move(nav.entities);
            for (auto& warning : nav.warnings)
                report_.Warn(std::move(warning));
            for (auto& error : nav.errors)
                report_.Fail(std::move(error));
            scriptModels_ = {nav.scriptModels.begin(), nav.scriptModels.end()};
            zoneGraph_ = std::move(nav.zoneGraph);
            dogStates_ = std::move(nav.dogStates);
            machineModels_ = std::move(nav.machineModels);
            // the map is king: every model its own zones ship converts, referenced or not
            content["map_shipped_models"] = shipped_.size();
            if (mod::IsFile(entitiesSource_))
                entityBoxModels_ = scriptModels_; // map-placed script_models: WaW collides each one
        }

        // --waw-source-fx: assets in no compiled WaW zone compile from the Mod Tools sources
        void BridgeStage::OpenModToolsSources()
        {
            if (!o_.wawSourceFx || !o_.wawRoot || !o_.t4Unlinker)
                return;
            const auto modTools = extract::WawSource::Locate(o_.wawModTools, o_.wawRoot, extract::ResourceRoot());
            source_.emplace(*modTools, *o_.wawRoot, *o_.t4Unlinker, o_.wawSourceDumps.value_or(o_.stage / "waw_source_dumps"));
            if (const auto problems = source_->Check(); !problems.empty())
            {
                report_.Fail("WaW Mod Tools sources unusable: " + mod::Join(problems, "; "));
                source_.reset();
            }
        }

        void BridgeStage::PortScripts()
        {
            Timed("Port map scripts and animation dependencies", [&]
            {
                if (!o_.wawStockScripts || !fs::exists(*o_.wawStockScripts))
                {
                    report_.Fail("no stock WaW scripts (--waw-stock-scripts): cannot tell the map's scripts from the WaW framework");
                    return 0;
                }
                // the patch zone loads after the map zone (nullopt marks its place) and replaces its scripts
                ScriptPortRequest request{o_.stage, projectRoot_, *o_.wawMapScript, *o_.bo2, {}, o_.iwdDirs.value_or(std::vector<fs::path>{}), roots_, {},
                                          *o_.wawStockScripts, stock_ ? &*stock_ : nullptr, source_ ? &*source_ : nullptr, o_.bo2StockPerks, o_.wawStockMap};
                request.roots.assign(o_.wawPatchScriptRoots.begin(), o_.wawPatchScriptRoots.end());
                request.roots.push_back(std::nullopt);
                request.roots.insert(request.roots.end(), o_.wawScriptRoots.begin(), o_.wawScriptRoots.end());
                if (o_.wawRoot)
                    request.stockVariantRoots.push_back(*o_.wawRoot / "raw");
                auto result = pipeline::PortScripts(request);
                if (Truthy(result.scripts))
                    report_.scripts = std::move(result.scripts);
                for (const auto& warning : result.warnings)
                    report_.Warn(warning);
                for (const auto& error : result.errors)
                    report_.Fail(error);
                roots_ = std::move(result.modelRoots);
                scriptModels_.insert(result.models.begin(), result.models.end());
                return 0;
            });
        }

        void BridgeStage::StageEffects(const std::vector<std::string>& names)
        {
            auto iwdDirs = o_.iwdDirs.value_or(std::vector<fs::path>{});
            if (o_.wawRoot)
                iwdDirs.push_back(*o_.wawRoot / "main");
            EffectsStageRequest request{names, roots_, projectRoot_, o_.stockMaterials, *o_.bo2, o_.techsetDump, iwdDirs,
                                        stock_ ? &*stock_ : nullptr, source_ ? &*source_ : nullptr, layeredVertexData_,
                                        bridge::WorkerCount(names.empty() ? 1 : names.size())};
            auto result = StageEffectsStep(request, *recovery_);
            for (const auto& [stem, metadata] : result.recovered.items())
                recovered_[stem] = metadata;
            for (const auto& warning : result.warnings)
                report_.Warn(warning);
            for (const auto& error : result.errors)
                report_.Fail(error);
            report_.materials.insert(report_.materials.end(), result.materials.begin(), result.materials.end());
            Extend(report_, "images", result.images);
            effects_.table = std::move(result.table);
            effects_.zoneFx = std::move(result.zoneFx);
            effects_.models = {result.models.begin(), result.models.end()};
            effects_.sounds = {result.sounds.begin(), result.sounds.end()};
            effects_.extraRoots = std::move(result.extraRoots);
            effects_.techsets = {result.techsets.begin(), result.techsets.end()};
        }

        // stage_fx: the script table of the effects the scripts load; the
        // rest are UNSUPPORTED_FX (skipped at runtime)
        void BridgeStage::WriteEffectTable(const std::vector<std::string>& names)
        {
            const auto& table = effects_.table;
            for (const auto& name : names)
                if (!effects_.table.contains(name))
                    report_.Warn("UNSUPPORTED_FX " + name + ": not converted (skipped at runtime)");
            report_.fxTable = effects_.table;
            files::WriteText(projectRoot_ / "maps/mp/waw/_waw2bo2_assets.gsc", gsc::port::AssetsSource(table, files::Utf8(o_.wawMapScript->stem())));
            files::WriteText(projectRoot_ / "mod_extra.zone", "");
        }

        // script animations load from mod.ff before the map's scripts; the game
        // resolves #using_animtree through the animtree rawfile
        void BridgeStage::AddAnimationZones()
        {
            const auto xanims = report_.Script("staged_xanims");
            const auto trees = report_.Script("staged_animtrees");
            if (!xanims.empty())
            {
                std::string lines;
                for (const auto& tree : trees)
                    lines += "rawfile,animtrees/" + tree.get<std::string>() + ".atr\n";
                for (const auto& name : xanims)
                    lines += "xanim," + name.get<std::string>() + "\n";
                AppendZone(projectRoot_, lines);
            }
            const auto movement = report_.Script("actor_movement", Json::object());
            if (Truthy(movement))
            {
                const auto tree = movement["tree"].get<std::string>();
                AppendZone(projectRoot_, "rawfile,animtrees/" + tree + ".atr\nrawfile,animstatedefs/" + tree + ".asd\n");
            }
            // native T6 trees of non-human source actors (WaW dogs on the T6 dog AI)
            const auto nativeTrees = report_.Script("native_actor_trees", Json::object());
            if (!Truthy(nativeTrees) || !o_.bo2)
                return;
            std::map<std::string, std::string> sorted;
            for (const auto& [tree, kind] : nativeTrees.items())
                sorted.emplace(tree, kind.get<std::string>());
            std::string lines;
            for (const auto& [base, kind] : sorted)
            {
                auto tree = base;
                const auto& templates = gsc::port::Api()["zombie_runtime"]["actor_templates"];
                const auto config = templates.contains(kind) ? templates[kind] : Json();
                std::string atr;
                if (config.is_object() && Truthy(config.value("owned_tree", Json())))
                {
                    OwnedActorTreeRequest request{projectRoot_, *o_.bo2, tree, config.at("owned_tree").get<std::string>(), {}, {}, roots_, nullptr};
                    const auto donors = config.value("add_states", Json::object());
                    for (const auto& [donor, states] : donors.items())
                        request.donorStates.emplace_back(donor, states.get<std::vector<std::string>>());
                    if (kind == "dog" && !dogStates_.empty())
                    {
                        request.sourceStates = dogStates_;
                        request.stock = stock_ ? &*stock_ : nullptr;
                    }
                    auto staged = StageOwnedActorTree(request);
                    for (const auto& error : staged.errors)
                        report_.Fail(error);
                    tree = std::move(staged.name);
                    atr = std::move(staged.tree);
                }
                else
                    atr = mod::Read(*o_.bo2 / "raw/animtrees" / files::FromUtf8(tree + ".atr"));
                lines += "rawfile,animtrees/" + tree + ".atr\nrawfile,animstatedefs/" + tree + ".asd\n";
                for (const auto& name : NativeTreeXanims(atr))
                    lines += "xanim," + name + "\n";
            }
            AppendZone(projectRoot_, lines);
        }

        // stage_models: placed models and their collision as T6 xmodel sources
        void BridgeStage::StageMapModels()
        {
            const auto extra = Union(Union(scriptModels_, Intersection(shipped_, resolved_)), clipModels_);
            Timed("Convert models and collision", [&]
            {
                Json placements = Json::array();
                for (const auto& model : staticModels_)
                    placements.push_back(Json{{"name", model.name}, {"origin", model.origin}, {"axis", model.axis}, {"scale", model.scale}, {"flags", model.flags},
                                              {"cullDist", model.cullDistance}, {"primaryLightIndex", model.primaryLight},
                                              {"reflectionProbeIndex", model.reflectionProbe}});
                const auto result = models::StageModels(projectRoot_, extract::ResourceRoot() / "vendor/OpenAssetToolsT6/raw/t6/partclassification.csv",
                                                        roots_, std::move(placements), {extra.begin(), extra.end()},
                                                        {entityBoxModels_.begin(), entityBoxModels_.end()});
                Extend(report_, "errors", result["errors"]);
                Extend(report_, "warnings", result["warnings"]);
                for (const auto& [name, entries] : result["content"].items())
                {
                    if (!report_.content.contains(name))
                        report_.content[name] = Json::array();
                    for (const auto& entry : entries)
                        report_.content[name].push_back(entry);
                }
                modelMaterials_ = NameSet(result["materials"]);
                return 0;
            });
        }

        // stage_dynents: the map's physics clutter and destructibles with their own presets
        void BridgeStage::StageDynents()
        {
            Timed("Convert effects", [&]
            {
                Names presets;
                for (const auto& entry : mapDynents_)
                    if (entry["physPreset"].is_object() && Truthy(entry["physPreset"].value("name", Json())))
                    {
                        auto name = entry["physPreset"]["name"].get<std::string>();
                        name.erase(0, name.find_first_not_of(','));
                        if (!name.empty())
                            presets.insert(name);
                    }
                std::map<std::string, fs::path> stockRoots;
                if (stock_)
                    for (const auto& name : presets)
                        if (const auto hit = stock_->RootFor("physpreset", name))
                            stockRoots.emplace(name, hit->second);
                const auto lookup = [&](const std::string& name) -> std::optional<fs::path>
                {
                    const auto hit = stockRoots.find(name);
                    return hit == stockRoots.end() ? std::nullopt : std::optional(hit->second);
                };
                const auto summary = bridge::Reported([&] { return physics::StageDynents(projectRoot_, mapDynents_, roots_, lookup, FX_OUTPUT_PREFIX); });
                if (Truthy(summary.value("resting", Json())))
                {
                    std::vector<std::string> resting;
                    for (const auto& name : summary["resting"])
                        resting.push_back(name.get<std::string>());
                    report_.Warn("DYNENT_SPAWN_BODY_DROPPED " + mod::Join(resting, ", ") +
                                                  ": WaW creates their physics body at spawn; the converted models carry no T6 physics geometry, "
                                                  "so they rest until hit (the client crashed creating it)");
                }
                report_.content["dynents"] = summary;
                return 0;
            });
        }

        // projectilecollision.recover: collision-only placements for missile-blocking props
        Names BridgeStage::RecoverProjectileModels()
        {
            const auto result = models::RecoverProjectileCollision({o_.gfx, o_.clip, projectRoot_, roots_});
            for (const auto& warning : result.warnings)
                report_.Warn(warning);
            if (result.entries)
                report_.content["projectile_prop_collision"] = *result.entries;
            return {result.names.begin(), result.names.end()};
        }

        // stage_fx_models: converted FX models under names of their own
        Names BridgeStage::StageEffectModels()
        {
            if (effects_.models.empty())
            {
                report_.content["fx_model_assets"] = Json::array();
                return {};
            }
            const auto result = models::StageEffectAssets(projectRoot_, {effects_.models.begin(), effects_.models.end()});
            for (const auto& error : result.errors)
                report_.Fail(error);
            report_.content["fx_model_assets"] = result.EntriesJson();
            Names names;
            for (const auto& entry : result.entries)
                names.insert(entry.name);
            return names;
        }

        // the world, model, HUD and weapon materials with their sources recovered first
        BridgeStage::StagedMaterials BridgeStage::StageAllMaterials()
        {
            const auto hudList = report_.Script("hud_materials");
            const auto hud = NameSet(hudList);
            Names names = Union(modelMaterials_, hud);
            for (const auto& surface : surfaces_)
                names.insert(surface.material);
            names = Union(names, NameSet(report_.content["physics_constraints"]["materials"]));
            Names weaponMaterials;
            if (weaponReport_)
            {
                weaponMaterials = NameSet((*weaponReport_)["models"]["materials"]);
                weaponMaterials = Union(weaponMaterials, NameSet((*weaponReport_)["dependencies"].value("material", Json::array())));
            }
            auto* stock = stock_ ? &*stock_ : nullptr;
            nativeFallbacks_ = bridge::RecoverMaterialSources(report_, Union(names, weaponMaterials), roots_, stock, source_ ? &*source_ : nullptr,
                                                              o_.stockMaterials, o_.bo2);
            if (o_.techsetDump)
                report_.content["source_impact_materials"] =
                    bridge::StageImpactMaterials(projectRoot_, roots_, *o_.techsetDump, stock, *recovery_, recovered_, layeredVertexData_);
            // primary lights keep their shadow maps only with the WaW shadow geometry
            const auto primaryLights = Sibling(o_.gfx, ".gfx.bin", ".primarylights.json");
            bridge::MaterialOptions options;
            options.falloffPlacement = bridge::LightFalloffPlacement(primaryLights, roots_);
            report_.content["light_falloff_placement"] = options.falloffPlacement ? Json(*options.falloffPlacement) : Json();
            options.lightShadows = !fs::exists(primaryLights) || shadowLights_;
            options.layeredVertexData = layeredVertexData_;
            options.nativeFallbacks = nativeFallbacks_;
            auto used = bridge::StageMaterials(report_, names, roots_, o_.stockMaterials, projectRoot_, o_.techsetDump, options);
            used = Union(used, effects_.techsets);
            Names hudImages;
            for (const auto& material : report_.materials)
                if (hud.contains(material.name))
                    for (const auto& texture : materials::ReadT6Material(projectRoot_ / files::FromUtf8(material.file)).textures)
                        if (!texture.image.empty())
                            hudImages.insert(texture.image);
            if (!hudImages.empty())
                bridge::WriteImageStreaming(projectRoot_, hudImages, true);
            return {hudList, std::move(used)};
        }

        // lightmaps.stage: a shared dual-encoding image for every lightmap page
        void BridgeStage::StageLightmaps(const Names& wawMaterials)
        {
            const auto result = Timed("Convert lightmaps", [&]
            {
                std::map<unsigned, textures::SurfacePage> pages;
                for (const auto& surface : surfaces_)
                {
                    auto& page = pages[surface.lightmap];
                    page.index = surface.lightmap;
                    ++page.surfaces;
                    page.materials.insert(surface.material);
                }
                const char* diagnostic = std::getenv("WAW2BO2_DIAG_PAGEID");
                textures::LightmapRequest request{projectRoot_, ImageFolders(roots_), {wawMaterials.begin(), wawMaterials.end()}, {},
                                                  diagnostic && *diagnostic, bridge::WorkerCount(61, true)};
                for (auto& [_, page] : pages)
                    request.surfacePages.push_back(std::move(page));
                return textures::StageLightmaps(request);
            });
            for (const auto& warning : result.warnings)
                report_.Warn(warning);
            Json pages = Json::array();
            for (const auto& page : result.pages)
                pages.push_back(page.ToJson());
            report_.content["lightmaps"] = Json{{"pages", std::move(pages)}, {"waw_encoded_materials", wawMaterials.size()}};
        }

        // the sky (the map's own dome, else BO2's with the WaW sky cube), then every image
        std::vector<std::string> BridgeStage::StageSkyAndImages(Names& used)
        {
            std::optional<std::string> skyTechset, skyImage = skyImage_;
            const auto sourceSky = SourceSkyModel(entitiesSource_);
            if (sourceSky && o_.bo2 && o_.techsetDump)
            {
                const auto sky = StageSourceSkybox({o_.project, projectRoot_, roots_, *sourceSky, *o_.techsetDump, *o_.bo2});
                report_.materials.insert(report_.materials.end(), sky.materials.begin(), sky.materials.end());
                report_.content["sky"] = sky.sky.ToJson();
                const Names skyMaterials(sky.sky.materials.begin(), sky.sky.materials.end());
                for (const auto& material : report_.materials)
                    if (skyMaterials.contains(material.name))
                    {
                        skyTechset = material.t6Techset;
                        break;
                    }
                skyImage.reset();
            }
            else
            {
                const auto sky = StageTemplateSkybox(report_, o_.project, projectRoot_, o_.bo2, skyImage, ImageFolders(roots_));
                skyTechset = sky.techset;
                skyImage = sky.image;
            }
            if (skyTechset)
            {
                used.insert(*skyTechset);
                const auto& content = report_.content;
                const auto skyMaterials = content.contains("sky") ? NameSet(content["sky"].value("materials", Json::array())) : Names{};
                for (const auto& material : report_.materials)
                    if (skyMaterials.contains(material.name))
                        used.insert(material.t6Techset);
            }
            const auto imageRoots = Concat({{projectRoot_ / SKY_SOURCE_DIR}, ImageFolders(roots_)});
            auto images = bridge::StageImages(report_, imageRoots, projectRoot_, imageIwds_, skyImage ? std::vector{*skyImage} : std::vector<std::string>{},
                                              *recovery_, recovered_);
            // the map's baked reflection probes join the zone's >ipak range like every image
            for (std::size_t index = 1; index < probeCount_; ++index)
                if (const auto name = "reflection_probe" + std::to_string(index); std::ranges::find(images, name) == images.end())
                    images.push_back(name);
            return images;
        }

        void BridgeStage::Tail(TailScripts scripts, std::vector<std::string> images, const Json& hudMaterials, const Names& zoneModels)
        {
            const auto wawRoots = MapScriptFolder();
            std::optional<fs::path> xwma = o_.xwmaDecoder.value_or(extract::ResourceRoot() / "tools/bin/xaudio_wma_decoder.exe");
            xwma = mod::IsFile(*xwma) ? std::optional(extract::Resolve(*xwma)) : std::nullopt;
            const auto resampler = audio::FindFfmpeg();
            std::optional<fs::path> soundDriver = o_.t6SoundDriver;
            if (!soundDriver && o_.techsetDump)
                soundDriver = *o_.techsetDump / "sounddriverglobals/singleton.w2bsdg";
            std::optional<fs::path> audioDecoder;
            if (o_.audioDecoder)
            {
                if (!mod::IsFile(*o_.audioDecoder))
                    throw StepError("SoundError", "missing requested audio decoder: " + Str(*o_.audioDecoder));
                audioDecoder = extract::Resolve(*o_.audioDecoder);
            }
            const auto mapStem = o_.wawMapScript ? std::optional(files::Utf8(o_.wawMapScript->stem())) : std::nullopt;
            const auto iwdDirs = o_.iwdDirs.value_or(std::vector<fs::path>{});

            TailInput input;
            input.project = o_.project;
            input.projectRoot = projectRoot_;
            input.stage = o_.stage;
            input.roots = roots_;
            if (o_.wawRoot)
                input.rawRoots.push_back(*o_.wawRoot / "raw");
            input.iwdDirs = iwdDirs;
            input.coverageRoots = Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots, Optional(o_.wawStockScripts)});
            input.bo2Root = o_.bo2;
            input.techsetDump = o_.techsetDump;
            input.t4Unlinker = o_.t4Unlinker;
            input.t6Unlinker = o_.t6Unlinker;
            input.wawMapStem = mapStem;
            input.images = std::move(images);
            input.hudMaterials = hudMaterials.get<std::vector<std::string>>();
            input.zoneModels.assign(zoneModels.begin(), zoneModels.end());
            input.zoneFx = effects_.zoneFx;
            input.ipak = !o_.noIpak;
            input.bo2StockPerks = o_.bo2StockPerks;
            input.hasWater = hasWater_;
            input.scripts = std::move(scripts);
            input.weapons = {weaponReport_.has_value(), o_.stockMaterials, nativeFallbacks_, layeredVertexData_, {}, Concat({o_.wawPatchScriptRoots, roots_}),
                             Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots})};
            for (const auto& [name, _] : effects_.table)
                input.weapons.convertedFx.insert(name);
            input.visionIwdDirs = iwdDirs;
            input.visionRoots = Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots, roots_, Optional(o_.wawStockScripts)});
            auto& sounds = input.sounds;
            sounds.mapStem = mapStem;
            if (o_.wawMapScript)
                sounds.ambRoots = Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots, roots_});
            sounds.ents = entitiesSource_;
            sounds.zoneSounds.assign(zoneSounds_.begin(), zoneSounds_.end());
            sounds.compiledSounds.assign(compiledSounds_.begin(), compiledSounds_.end());
            sounds.iwdDirs = imageIwds_;
            sounds.audioDecoder = audioDecoder;
            sounds.xwmaHelper = xwma;
            if (!resampler.empty())
                sounds.resampler = resampler;
            sounds.t6SoundDriver = soundDriver;
            sounds.approximateCurves = o_.approximateSoundCurves;
            input.lighting = {Sibling(o_.gfx, ".gfx.bin", ""), o_.clip, shadowLights_};
            BridgeTail(std::move(input), report_, stock_ ? &*stock_ : nullptr, *recovery_);
        }
        const BridgeReport& BridgeStage::Run()
        {
            OpenSources();
            StageGeometry();
            if (!o_.wawMapScript || !o_.bo2)
                throw StepError("StageError", "the native bridge stage needs --waw-map-script and --bo2");
            OpenModToolsSources();
            PortScripts();
            const auto listened = NameSet(report_.Script("client_listener_strings"));
            const auto entitiesJson = projectRoot_ / "BSP/entities.json";
            if (!listened.empty() && mod::IsFile(entitiesJson))
                if (const auto networked = NetworkClientModels(entitiesJson, listened); !networked.empty())
                    report_.Warn("CLIENT_MODELS_NETWORKED " + mod::Join(networked, ", ") +
                                                  ": client-side script_models driven by WaW client listeners, which the port runs on the server");
            std::vector<std::string> fxNames;
            for (const auto& name : report_.Script("fx"))
                fxNames.push_back(name.get<std::string>());
            // the dynents' destroy effects convert with the map's own
            Names destroyFx;
            for (const auto& entry : mapDynents_)
                if (Truthy(entry["destroyFx"]))
                    destroyFx.insert(entry["destroyFx"].get<std::string>());
            for (const auto& name : Difference(destroyFx, Names(fxNames.begin(), fxNames.end())))
                fxNames.push_back(name);
            // weapons first: their resolution widens the roots (stock WaW dumps
            // for script models too) and their effects join the map zone's conversion
            auto* stock = stock_ ? &*stock_ : nullptr;
            weaponReport_ = bridge::StageWeapons(report_, roots_, projectRoot_, compiledWeapons_, scriptModels_, stock, compiledSounds_);
            auto effectNames = fxNames;
            if (weaponReport_)
                for (const auto& name : Difference(NameSet((*weaponReport_)["dependencies"].value("fx", Json::array())), Names(fxNames.begin(), fxNames.end())))
                    effectNames.push_back(name);
            if (!stock_)
                report_.Warn("no --waw-root/--t4-unlinker: assets the map only references cannot be looked up in the stock WaW zones");
            StageEffects(effectNames);
            compiledSounds_ = Union(compiledSounds_, effects_.sounds);
            roots_.insert(roots_.end(), effects_.extraRoots.begin(), effects_.extraRoots.end());
            scriptModels_ = Union(scriptModels_, effects_.models);
            WriteEffectTable(fxNames);
            AddAnimationZones();

            // the clipmap's dynents reference their xmodel in the map zone too
            for (const auto& entry : mapDynents_)
                clipModels_.insert(entry["xModel"].get<std::string>());
            Names worldModels;
            for (const auto& model : staticModels_)
                worldModels.insert(model.name);
            // entity placements need the same complete lookup as script-only models
            resolved_ = bridge::RecoverScriptModels(report_.scripts, report_.warnings, Union(Union(Union(scriptModels_, shipped_), clipModels_), worldModels),
                                                    roots_, stock, source_ ? &*source_ : nullptr);
            if (mod::IsFile(entitiesJson))
            {
                // an entity naming a model no WaW zone or source has: WaW loads
                // its default model; the entity stays for scripts, without a model
                const auto absent = Difference(Difference(Difference(Difference(PlacedModels(entitiesJson), resolved_), clipModels_), worldModels), machineModels_);
                if (!absent.empty())
                {
                    const auto count = StripModels(entitiesJson, absent);
                    scriptModels_ = Difference(scriptModels_, absent);
                    entityBoxModels_ = Difference(entityBoxModels_, absent);
                    report_.Warn("ENTITY_MODEL_MISSING " + mod::ListRepr(std::vector<std::string>(absent.begin(), absent.end())) +
                                                  ": in no WaW zone or source; " + std::to_string(count) +
                                                  " entities keep no model (WaW draws its default model)");
                }
            }
            StageMapModels();
            StageDynents();
            const auto projectileModels = RecoverProjectileModels();
            const auto fxModels = StageEffectModels();
            auto [hudMaterials, used] = StageAllMaterials();

            // shared dual-encoding lightmap pages: native decals inherit the
            // receiving surface's page, so native and WaW readers share its image
            Names wawLightmapMaterials;
            for (const auto& material : report_.materials)
                if (material.shaderRuntime.is_object() && material.shaderRuntime.value("lightmap", Json()) == "waw")
                    wawLightmapMaterials.insert(material.WawName());
            StageLightmaps(wawLightmapMaterials);
            const auto images = StageSkyAndImages(used);

            TailScripts scripts{{used.begin(), used.end()}, *o_.wawMapScript, std::nullopt, std::nullopt, std::nullopt};
            if (o_.scriptTemplate && o_.scriptTemplateName)
                scripts.requiredTemplate = gsc::port::ScriptTemplate{*o_.scriptTemplate, *o_.scriptTemplateName};
            if (zoneGraph_)
            {
                scripts.dropZoneManager = true;
                scripts.zoneGraph = *zoneGraph_;
            }
            const auto zoneModels = Union(Union(Union(scriptModels_, Intersection(shipped_, resolved_)), fxModels), projectileModels);
            Tail(std::move(scripts), images, hudMaterials, zoneModels);
            return report_;
        }

        // cli._stage_bridge's summary
        int PrintSummary(const BridgeReport& report, const std::string& project)
        {
            std::printf("collision: %s\n", py::JsonDumpsDefault(report.collision).c_str());
            const auto degraded = std::ranges::count_if(report.materials, [](const MaterialRecord& m) { return m.notes && !m.notes->empty(); });
            std::printf("materials: %zu written, %zu with degradations (see zone_raw/%s/bridge_stage.report.json)\n", report.materials.size(),
                        static_cast<std::size_t>(degraded), project.c_str());
            std::printf("images: %zu IWI written; technique sets used: %zu\n", report.images.size(), report.techsets.size());
            for (const auto& warning : report.warnings)
                std::printf("warning: %s\n", warning.c_str());
            std::fflush(stdout);
            const auto& errors = report.errors;
            for (std::size_t i = 0; i < errors.size() && i < 40; ++i)
                std::fprintf(stderr, "error: %s\n", errors[i].c_str());
            if (errors.size() > 40)
                std::fprintf(stderr, "... %zu more errors in the report\n", errors.size() - 40);
            std::fflush(stderr);
            return errors.empty() ? 0 : 1;
        }
    } // namespace

    BridgeStageOptions BridgeStageOptions::Parse(const std::vector<std::string>& args)
    {
        BridgeStageOptions o;
        std::vector<std::string> positional;
        const std::map<std::string, std::optional<fs::path>*> paths{
            {"--waw-map-script", &o.wawMapScript},     {"--techset-dump", &o.techsetDump},   {"--bo2", &o.bo2},
            {"--script-template", &o.scriptTemplate},  {"--t6-unlinker", &o.t6Unlinker},     {"--waw-root", &o.wawRoot},
            {"--t4-unlinker", &o.t4Unlinker},          {"--waw-stock-dumps", &o.wawStockDumps}, {"--audio-decoder", &o.audioDecoder},
            {"--xwma-decoder", &o.xwmaDecoder},        {"--waw-mod-tools", &o.wawModTools},  {"--waw-source-dumps", &o.wawSourceDumps},
            {"--waw-stock-scripts", &o.wawStockScripts}, {"--t6-sound-driver", &o.t6SoundDriver}};
        const std::map<std::string, std::vector<fs::path>*> lists{{"--extra-root", &o.extraRoots},
                                                                   {"--map-root", &o.mapRoots},
                                                                   {"--waw-script-root", &o.wawScriptRoots},
                                                                   {"--waw-patch-script-root", &o.wawPatchScriptRoots}};
        const std::map<std::string, bool*> flags{{"--no-ipak", &o.noIpak},
                                                 {"--waw-source-fx", &o.wawSourceFx},
                                                 {"--approximate-sound-curves", &o.approximateSoundCurves},
                                                 {"--bo2-stock-perks", &o.bo2StockPerks},
                                                 {"--waw-stock-map", &o.wawStockMap}};
        std::optional<fs::path> gfx, clip, stockMaterials;
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            const auto& arg = args[i];
            const auto value = [&]() -> const std::string&
            {
                if (i + 1 >= args.size())
                    throw StepError("ArgumentError", "argument " + arg + ": expected one argument");
                return args[++i];
            };
            if (const auto flag = flags.find(arg); flag != flags.end())
                *flag->second = true;
            else if (const auto path = paths.find(arg); path != paths.end())
                *path->second = Resolved(value());
            else if (const auto list = lists.find(arg); list != lists.end())
                list->second->push_back(Resolved(value()));
            else if (arg == "--iwd-dir")
            {
                if (!o.iwdDirs)
                    o.iwdDirs.emplace();
                o.iwdDirs->push_back(Resolved(value()));
            }
            else if (arg == "--gfx")
                gfx = Resolved(value());
            else if (arg == "--clip")
                clip = Resolved(value());
            else if (arg == "--stock-materials")
                stockMaterials = Resolved(value());
            else if (arg == "--script-template-name")
                o.scriptTemplateName = value();
            else if (arg.starts_with("--"))
                throw StepError("ArgumentError", "unrecognized arguments: " + arg);
            else
                positional.push_back(arg);
        }
        if (positional.size() != 2 || !gfx || !clip || !stockMaterials)
            throw StepError("ArgumentError", "stage-bridge needs <stage> <project> --gfx --clip --stock-materials");
        o.stage = Resolved(positional[0]);
        o.project = positional[1];
        o.gfx = *gfx;
        o.clip = *clip;
        o.stockMaterials = *stockMaterials;
        return o;
    }

    int StageBridge(const BridgeStageOptions& options)
    {
        if (options.wawSourceFx && !extract::WawSource::Locate(options.wawModTools, options.wawRoot, extract::ResourceRoot()))
            throw StepError("ArgumentError", "--waw-source-fx needs the WaW Mod Tools (--waw-mod-tools; none found automatically)");
        const timeline::Span span("cli stage-bridge");
        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        BridgeStage stage(options);
        const auto report = stage.Run();
        return PrintSummary(report, options.project);
    }
} // namespace w2b::pipeline
