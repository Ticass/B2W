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
#include "core/timeline.h"
#include "effects_stage.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
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

namespace w2b::pipeline
{
    using bridge::AppendZone;
    using bridge::Call;
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

        // a native step's error keys, raised as the Python errors they report
        void RaiseReported(const Json& result)
        {
            static const std::pair<const char*, const char*> kinds[] = {{"stage_error", "StageError"}, {"weapon_error", "WeaponError"},
                                                                        {"invalid", "ValueError"},     {"syntax_error", "GscSyntaxError"},
                                                                        {"path_error", "PathError"}};
            for (const auto& [key, kind] : kinds)
                if (result.contains(key))
                    throw StepError(kind, result[key].get<std::string>());
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
                if (entity.value("classname", std::string()) == "worldspawn")
                    return entity.contains("skyboxmodel") ? std::optional(entity["skyboxmodel"].get<std::string>()) : std::nullopt;
            return std::nullopt;
        }

        // entities.placed_models / strip_models / network_client_models over BSP/entities.json
        Names PlacedModels(const fs::path& entitiesJson)
        {
            Names names;
            const auto data = mod::Json(entitiesJson);
            for (const auto& entity : data.at("entities"))
                if (entity.contains("model") && Truthy(entity["model"]) && !entity["model"].get<std::string>().starts_with('*'))
                    names.insert(entity["model"].get<std::string>());
            return names;
        }

        std::size_t StripModels(const fs::path& entitiesJson, const Names& names)
        {
            auto data = mod::Json(entitiesJson);
            std::size_t count = 0;
            for (auto& entity : data["entities"])
                if (entity.contains("model") && entity["model"].is_string() && names.contains(entity["model"].get<std::string>()))
                {
                    entity.erase("model");
                    ++count;
                }
            files::WriteText(entitiesJson, py::JsonDumps(data, 1) + "\n");
            return count;
        }

        // client-side script_models (spawnflag 2) whose targetname a WaW client
        // listener drives become server entities: the port runs those listeners on the server
        std::vector<std::string> NetworkClientModels(const fs::path& entitiesJson, const Names& listened)
        {
            constexpr long long CLIENT_SIDE_MODEL = 2;
            auto data = mod::Json(entitiesJson);
            std::map<std::string, int> changed;
            for (auto& entity : data["entities"])
            {
                long long flags = 0;
                bool overflow = false;
                const auto text = entity.value("spawnflags", std::string("0"));
                if (!text.empty())
                    py::ParseInt(text, flags, overflow);
                const auto target = entity.value("targetname", std::string());
                if (entity.value("classname", std::string()) == "script_model" && (flags & CLIENT_SIDE_MODEL) && entity.contains("targetname") &&
                    listened.contains(target))
                {
                    entity["spawnflags"] = std::to_string(flags & ~CLIENT_SIDE_MODEL);
                    ++changed[target];
                }
            }
            if (!changed.empty())
                files::WriteText(entitiesJson, py::JsonDumps(data, 1) + "\n");
            std::vector<std::string> lines;
            for (const auto& [name, count] : changed)
                lines.push_back(name + " (" + std::to_string(count) + ")");
            return lines;
        }

        // ------------------------------------------------------------ the stage

        // what the effects pass hands on (EffectsResult)
        struct Effects
        {
            Json table = Json::object(); // script-loaded WaW effect -> converted name
            Json zoneFx = Json::array();
            Names models, sounds, techsets;
            std::vector<fs::path> extraRoots;
        };

        class BridgeStage
        {
        public:
            explicit BridgeStage(const BridgeStageOptions& options) : o_(options) {}

            Json Run();

        private:
            void OpenSources();
            void StageGeometry();
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
            void StageLightmaps(const Json& wawMaterials);
            std::vector<std::string> StageSkyAndImages(Names& used);
            Json Tail(Json scripts, const std::vector<std::string>& images, const Json& hudMaterials, const Names& zoneModels);

            std::vector<fs::path> MapScriptFolder() const
            {
                return o_.wawMapScript ? std::vector<fs::path>{o_.wawMapScript->parent_path().parent_path()} : std::vector<fs::path>{};
            }

            const BridgeStageOptions& o_;
            fs::path projectRoot_;
            Json report_;
            std::vector<fs::path> roots_, imageIwds_;
            std::optional<extract::StockWaw> stock_;
            std::unique_ptr<textures::IwdRecovery> recovery_;
            Json recovered_ = Json::object(); // IWD images decoded so far (wavelet.recovered)

            // the geometry stage's results
            Json surfaces_, staticModels_, mapDynents_, navigation_;
            bool shadowLights_ = false, hasWater_ = false, layeredVertexData_ = false;
            std::optional<std::string> skyImage_;
            std::size_t probeCount_ = 0;
            Names clipModels_;

            // the navigation stage's results
            Names compiledWeapons_, zoneSounds_, shipped_, scriptModels_, entityBoxModels_, machineModels_, compiledSounds_;
            fs::path entitiesSource_;
            Json zoneGraph_, dogStates_;

            std::optional<Json> weaponReport_;
            Effects effects_;
            Names resolved_, modelMaterials_;
            std::map<std::string, fs::path> nativeFallbacks_;
        };

        void BridgeStage::OpenSources()
        {
            projectRoot_ = o_.stage / "zone_raw" / files::FromUtf8(o_.project);
            fs::create_directories(projectRoot_);
            report_ = Json{{"project", o_.project}, {"materials", Json::array()}, {"images", Json::array()}, {"techsets", Json::array()},
                           {"collision", Json::object()}, {"entities", Json::object()}, {"scripts", Json::object()}, {"content", Json::object()},
                           {"fx_table", Json::object()}, {"errors", Json::array()}, {"warnings", Json::array()}};
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
        }

        void BridgeStage::StageGeometry()
        {
            const auto traversalRoots = Concat({o_.wawPatchScriptRoots, MapScriptFolder(), o_.wawScriptRoots, roots_, Optional(o_.wawStockScripts)});
            Json levelWeapons = Json::array();
            if (stock_ && stock_->Index().contains("common") && stock_->Index()["common"].contains("weapon"))
                levelWeapons = stock_->Index()["common"]["weapon"];
            const Json navigation{{"project", o_.project}, {"stage", Str(o_.stage)}, {"bo2_root", PathOrNull(o_.bo2)},
                                  {"waw_map_script", PathOrNull(o_.wawMapScript)}, {"traversal_roots", Paths(traversalRoots)},
                                  {"map_roots", Paths(o_.mapRoots)}, {"level_weapons", levelWeapons}, {"bo2_stock_perks", o_.bo2StockPerks},
                                  {"respawn_exclusion", gsc::port::Api()["zombie_runtime"]["source_respawn"]["native_exclusion_tag"]}};
            const Json request{{"gfx", Str(o_.gfx)}, {"clip", Str(o_.clip)}, {"project_root", Str(projectRoot_)}, {"roots", Paths(roots_)},
                               {"stock_waw", bridge::StockRequest(stock_ ? &*stock_ : nullptr)}, {"compensated", true},
                               {"probe_source", Str(Sibling(o_.gfx, ".gfx.bin", ".reflectionprobes.json"))},
                               {"visibility_source", Str(Sibling(o_.gfx, ".gfx.bin", ".visibility.json"))},
                               {"image_roots", Paths(ImageFolders(roots_))},
                               {"dynents", Str(Sibling(o_.clip, ".clip.bin", ".dynents.json"))},
                               {"constraints", Str(Sibling(o_.clip, ".clip.bin", ".constraints.json"))}, {"navigation", navigation}};
            const auto staged = Timed("Convert world geometry", [&] { return StageBridgeGeometry(request); });
            RaiseReported(staged);
            bridge::AdoptPrepared(stock_ ? &*stock_ : nullptr, staged);
            const auto& probes = staged["probes"];
            Extend(report_, "images", probes["images"]);
            Extend(report_, "warnings", probes["warnings"]);
            Extend(report_, "errors", probes["errors"]);
            auto& content = report_["content"];
            if (probes.contains("content"))
                content["reflection_probes"] = probes["content"];
            const auto warningIndex = report_["warnings"].size();
            content["visibility"] = nullptr;
            content["physics_constraints"] = staged["physics"]["constraints"];
            layeredVertexData_ = staged["layered"].get<bool>();
            for (const auto& [key, value] : staged["content"].items())
                content[key] = value;
            if (content["visibility"].is_null())
                content.erase("visibility");
            report_["collision"] = staged["collision"];
            Extend(report_, "warnings", staged["warnings"]);
            if (Truthy(staged["visibility_warning"]))
                report_["warnings"].insert(report_["warnings"].begin() + static_cast<std::ptrdiff_t>(warningIndex), staged["visibility_warning"]);
            Extend(report_, "errors", staged["errors"]);
            const auto& world = staged["world"];
            surfaces_ = world["surfaces"];
            staticModels_ = world["static_models"];
            shadowLights_ = Truthy(world["shadow_lights"]);
            if (staged["sky"].is_string())
                skyImage_ = staged["sky"].get<std::string>();
            probeCount_ = probes.contains("kept") ? probes["kept"].size() : 0;
            mapDynents_ = staged["physics"]["dynents"];
            clipModels_ = NameSet(staged["clip_models"]);
            hasWater_ = Truthy(staged["has_water"]);
            navigation_ = staged["navigation"];

            const auto& inventory = navigation_["inventory"];
            compiledWeapons_ = NameSet(inventory["compiled_weapons"]);
            zoneSounds_ = NameSet(inventory["zone_sounds"]);
            shipped_ = NameSet(inventory["shipped_models"]);
            entitiesSource_ = files::FromUtf8(navigation_["entity_source"].get<std::string>());
            report_["entities"] = navigation_["entities"];
            Extend(report_, "warnings", navigation_["warnings"]);
            Extend(report_, "errors", navigation_["errors"]);
            scriptModels_ = NameSet(navigation_["script_models"]);
            zoneGraph_ = navigation_["zone_graph"];
            dogStates_ = navigation_["dog_states"];
            machineModels_ = NameSet(navigation_["bo2_machine_models"]);
            // the map is king: every model its own zones ship converts, referenced or not
            content["map_shipped_models"] = shipped_.size();
            if (mod::IsFile(entitiesSource_))
                entityBoxModels_ = scriptModels_; // map-placed script_models: WaW collides each one
        }

        void BridgeStage::PortScripts()
        {
            Timed("Port map scripts and animation dependencies", [&]
            {
                if (!o_.wawStockScripts || !fs::exists(*o_.wawStockScripts))
                {
                    report_["errors"].push_back("no stock WaW scripts (--waw-stock-scripts): cannot tell the map's scripts from the WaW framework");
                    return 0;
                }
                // the patch zone loads after the map zone (null marks its place) and replaces its scripts
                Json scriptRoots = Paths(o_.wawPatchScriptRoots);
                scriptRoots.push_back(nullptr);
                for (const auto& root : o_.wawScriptRoots)
                    scriptRoots.push_back(Str(root));
                std::vector<fs::path> variantRoots;
                if (o_.wawRoot)
                    variantRoots.push_back(*o_.wawRoot / "raw");
                const auto result = Call(pipeline::PortScripts,
                                         Json{{"stage", Str(o_.stage)}, {"project_root", Str(projectRoot_)}, {"waw_map_script", Str(*o_.wawMapScript)},
                                              {"bo2_root", Str(*o_.bo2)}, {"roots", scriptRoots}, {"iwd_dirs", Paths(o_.iwdDirs.value_or(std::vector<fs::path>{}))},
                                              {"stock", Str(*o_.wawStockScripts)}, {"model_roots", Paths(roots_)},
                                              {"stock_waw", bridge::StockRequest(stock_ ? &*stock_ : nullptr)}, {"source_waw", false},
                                              {"bo2_stock_perks", o_.bo2StockPerks}, {"waw_stock_map", o_.wawStockMap},
                                              {"stock_variant_roots", Paths(variantRoots)}});
                if (Truthy(result["scripts"]))
                    report_["scripts"] = result["scripts"];
                Extend(report_, "warnings", result["warnings"]);
                Extend(report_, "errors", result["errors"]);
                roots_ = bridge::PathList(result["model_roots"]);
                bridge::AdoptPrepared(stock_ ? &*stock_ : nullptr, result);
                scriptModels_ = Union(scriptModels_, NameSet(result["models"]));
                return 0;
            });
        }

        void BridgeStage::StageEffects(const std::vector<std::string>& names)
        {
            auto iwdDirs = o_.iwdDirs.value_or(std::vector<fs::path>{});
            if (o_.wawRoot)
                iwdDirs.push_back(*o_.wawRoot / "main");
            auto result = StageEffectsStep(Json{{"names", names}, {"roots", Paths(roots_)}, {"project_root", Str(projectRoot_)},
                                                {"stock_materials", Str(o_.stockMaterials)}, {"techset_dump", PathOrNull(o_.techsetDump)},
                                                {"bo2_root", Str(*o_.bo2)}, {"iwd_dirs", Paths(iwdDirs)},
                                                {"stock", bridge::StockRequest(stock_ ? &*stock_ : nullptr)},
                                                {"layered_vertex_data", layeredVertexData_}, {"workers", bridge::WorkerCount(names.empty() ? 1 : names.size())}},
                                           *recovery_);
            bridge::AdoptPrepared(stock_ ? &*stock_ : nullptr, result);
            for (const auto& [stem, metadata] : result.value("recovered", Json::object()).items())
                recovered_[stem] = metadata;
            Extend(report_, "warnings", result["warnings"]);
            Extend(report_, "errors", result["errors"]);
            Extend(report_, "materials", result["materials"]);
            Extend(report_, "images", result["images"]);
            effects_.table = result["table"];
            effects_.zoneFx = result["zone_fx"];
            effects_.models = NameSet(result["models"]);
            effects_.sounds = NameSet(result["sounds"]);
            effects_.extraRoots = bridge::PathList(result["extra_roots"]);
            effects_.techsets = NameSet(result["techsets"]);
        }

        // stage_fx: the script table of the effects the scripts load; the
        // rest are UNSUPPORTED_FX (skipped at runtime)
        void BridgeStage::WriteEffectTable(const std::vector<std::string>& names)
        {
            std::map<std::string, std::string> table;
            for (const auto& [name, converted] : effects_.table.items())
                table.emplace(name, converted.get<std::string>());
            for (const auto& name : names)
                if (!effects_.table.contains(name))
                    report_["warnings"].push_back("UNSUPPORTED_FX " + name + ": not converted (skipped at runtime)");
            report_["fx_table"] = effects_.table;
            files::WriteText(projectRoot_ / "maps/mp/waw/_waw2bo2_assets.gsc", gsc::port::AssetsSource(table, files::Utf8(o_.wawMapScript->stem())));
            files::WriteText(projectRoot_ / "mod_extra.zone", "");
        }

        // script animations load from mod.ff before the map's scripts; the game
        // resolves #using_animtree through the animtree rawfile
        void BridgeStage::AddAnimationZones()
        {
            const auto xanims = Script(report_, "staged_xanims");
            const auto trees = Script(report_, "staged_animtrees");
            if (!xanims.empty())
            {
                std::string lines;
                for (const auto& tree : trees)
                    lines += "rawfile,animtrees/" + tree.get<std::string>() + ".atr\n";
                for (const auto& name : xanims)
                    lines += "xanim," + name.get<std::string>() + "\n";
                AppendZone(projectRoot_, lines);
            }
            const auto movement = Script(report_, "actor_movement", Json::object());
            if (Truthy(movement))
            {
                const auto tree = movement["tree"].get<std::string>();
                AppendZone(projectRoot_, "rawfile,animtrees/" + tree + ".atr\nrawfile,animstatedefs/" + tree + ".asd\n");
            }
            // native T6 trees of non-human source actors (WaW dogs on the T6 dog AI)
            const auto nativeTrees = Script(report_, "native_actor_trees", Json::object());
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
                    const auto states = kind == "dog" ? dogStates_ : Json::object();
                    const auto staged = Call(StageOwnedActorTreeRequest,
                                             Json{{"project_root", Str(projectRoot_)}, {"bo2_root", Str(*o_.bo2)}, {"base", tree}, {"config", config},
                                                  {"source_states", Truthy(states) ? states : Json::object()}, {"roots", Paths(roots_)},
                                                  {"stock_waw", stock_ && Truthy(states) ? bridge::StockRequest(&*stock_) : Json()}});
                    if (staged.contains("error"))
                        throw StepError(staged["error_kind"] == "TraversalError" ? "TraversalError" : "StageError", staged["error"].get<std::string>());
                    bridge::AdoptPrepared(stock_ ? &*stock_ : nullptr, staged);
                    Extend(report_, "errors", staged["errors"]);
                    tree = staged["name"].get<std::string>();
                    atr = staged["tree"].get<std::string>();
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
                    placements.push_back(Json{{"name", model[0]}, {"origin", model[1]}, {"axis", model[2]}, {"scale", model[3]}, {"flags", model[4]},
                                              {"cullDist", model[5]}, {"primaryLightIndex", model[6]}, {"reflectionProbeIndex", model[7]}});
                const auto result = Call(models::Stage, Json{{"project", Str(projectRoot_)},
                                                             {"partCsv", Str(extract::ResourceRoot() / "vendor/OpenAssetToolsT6/raw/t6/partclassification.csv")},
                                                             {"placements", placements}, {"roots", Paths(roots_)}, {"extraModels", SortedNames(extra)},
                                                             {"boxModels", SortedNames(entityBoxModels_)}});
                Extend(report_, "errors", result["errors"]);
                Extend(report_, "warnings", result["warnings"]);
                for (const auto& [name, entries] : result["content"].items())
                {
                    if (!report_["content"].contains(name))
                        report_["content"][name] = Json::array();
                    for (const auto& entry : entries)
                        report_["content"][name].push_back(entry);
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
                Json stockRoots = Json::object();
                if (stock_)
                    for (const auto& name : presets)
                        if (const auto hit = stock_->RootFor("physpreset", name))
                            stockRoots[name] = Str(hit->second);
                const auto response = Call(physics::Run, Json{{"operation", "stage-dynents"}, {"project", Str(projectRoot_)}, {"entries", mapDynents_},
                                                              {"roots", Paths(roots_)}, {"stockRoots", stockRoots}, {"effectPrefix", FX_OUTPUT_PREFIX}});
                if (response.contains("weaponError"))
                    throw StepError("WeaponError", response["weaponError"].get<std::string>());
                if (response.contains("invalid"))
                    throw StepError("ValueError", response["invalid"].get<std::string>());
                const auto& summary = response["result"];
                if (Truthy(summary.value("resting", Json())))
                {
                    std::vector<std::string> resting;
                    for (const auto& name : summary["resting"])
                        resting.push_back(name.get<std::string>());
                    report_["warnings"].push_back("DYNENT_SPAWN_BODY_DROPPED " + mod::Join(resting, ", ") +
                                                  ": WaW creates their physics body at spawn; the converted models carry no T6 physics geometry, "
                                                  "so they rest until hit (the client crashed creating it)");
                }
                report_["content"]["dynents"] = summary;
                return 0;
            });
        }

        // projectilecollision.recover: collision-only placements for missile-blocking props
        Names BridgeStage::RecoverProjectileModels()
        {
            const auto result = Call(models::RecoverProjectileCollision,
                                     Json{{"gfx", Str(o_.gfx)}, {"clip", Str(o_.clip)}, {"project_root", Str(projectRoot_)}, {"roots", Paths(roots_)}});
            Extend(report_, "warnings", result["warnings"]);
            if (!result["entries"].is_null())
                report_["content"]["projectile_prop_collision"] = result["entries"];
            return NameSet(result["names"]);
        }

        // stage_fx_models: converted FX models under names of their own
        Names BridgeStage::StageEffectModels()
        {
            if (effects_.models.empty())
            {
                report_["content"]["fx_model_assets"] = Json::array();
                return {};
            }
            const auto result = Call(models::StageEffectAssetsRequest, Json{{"project", Str(projectRoot_)}, {"names", SortedNames(effects_.models)}});
            Extend(report_, "errors", result["errors"]);
            report_["content"]["fx_model_assets"] = result["entries"];
            Names names;
            for (const auto& entry : result["entries"])
                names.insert(entry["name"].get<std::string>());
            return names;
        }

        // the world, model, HUD and weapon materials with their sources recovered first
        BridgeStage::StagedMaterials BridgeStage::StageAllMaterials()
        {
            const auto hudList = Script(report_, "hud_materials");
            const auto hud = NameSet(hudList);
            Names names = Union(modelMaterials_, hud);
            for (const auto& surface : surfaces_)
                names.insert(surface[0].get<std::string>());
            names = Union(names, NameSet(report_["content"]["physics_constraints"]["materials"]));
            Names weaponMaterials;
            if (weaponReport_)
            {
                weaponMaterials = NameSet((*weaponReport_)["models"]["materials"]);
                weaponMaterials = Union(weaponMaterials, NameSet((*weaponReport_)["dependencies"].value("material", Json::array())));
            }
            auto* stock = stock_ ? &*stock_ : nullptr;
            nativeFallbacks_ = bridge::RecoverMaterialSources(report_, Union(names, weaponMaterials), roots_, stock, o_.stockMaterials, o_.bo2);
            if (o_.techsetDump)
                report_["content"]["source_impact_materials"] =
                    bridge::StageImpactMaterials(projectRoot_, roots_, *o_.techsetDump, stock, *recovery_, recovered_, layeredVertexData_);
            // primary lights keep their shadow maps only with the WaW shadow geometry
            const auto primaryLights = Sibling(o_.gfx, ".gfx.bin", ".primarylights.json");
            bridge::MaterialOptions options;
            options.falloffPlacement = bridge::LightFalloffPlacement(primaryLights, roots_);
            report_["content"]["light_falloff_placement"] = options.falloffPlacement ? Json(*options.falloffPlacement) : Json();
            options.lightShadows = !fs::exists(primaryLights) || shadowLights_;
            options.layeredVertexData = layeredVertexData_;
            options.nativeFallbacks = nativeFallbacks_;
            auto used = bridge::StageMaterials(report_, names, roots_, o_.stockMaterials, projectRoot_, o_.techsetDump, options);
            used = Union(used, effects_.techsets);
            Names hudImages;
            for (const auto& material : report_["materials"])
                if (hud.contains(material["name"].get<std::string>()))
                    for (const auto& texture : mod::Json(projectRoot_ / files::FromUtf8(material["file"].get<std::string>())).value("textures", Json::array()))
                        if (Truthy(texture.value("image", Json())))
                            hudImages.insert(texture["image"].get<std::string>());
            if (!hudImages.empty())
                bridge::WriteImageStreaming(projectRoot_, hudImages, true);
            return {hudList, std::move(used)};
        }

        // lightmaps.stage: a shared dual-encoding image for every lightmap page
        void BridgeStage::StageLightmaps(const Json& wawMaterials)
        {
            const auto result = Timed("Convert lightmaps", [&]
            {
                std::map<long long, std::pair<long long, Names>> pages;
                for (const auto& surface : surfaces_)
                {
                    auto& [count, materials] = pages[surface[1].get<long long>()];
                    ++count;
                    materials.insert(surface[0].get<std::string>());
                }
                Json surfacePages = Json::array();
                for (const auto& [index, page] : pages)
                    surfacePages.push_back(Json::array({index, page.first, SortedNames(page.second)}));
                const char* diagnostic = std::getenv("WAW2BO2_DIAG_PAGEID");
                return Call(textures::StageLightmaps, Json{{"project", Str(projectRoot_)}, {"image_roots", Paths(ImageFolders(roots_))},
                                                           {"waw_materials", wawMaterials}, {"surface_pages", surfacePages},
                                                           {"diagnostic", diagnostic && *diagnostic}, {"workers", bridge::WorkerCount(61, true)}});
            });
            Extend(report_, "errors", result["errors"]);
            Extend(report_, "warnings", result["warnings"]);
            report_["content"]["lightmaps"] = Json{{"pages", result["pages"]}, {"waw_encoded_materials", wawMaterials.size()}};
        }

        // the sky (the map's own dome, else BO2's with the WaW sky cube), then every image
        std::vector<std::string> BridgeStage::StageSkyAndImages(Names& used)
        {
            std::optional<std::string> skyTechset, skyImage = skyImage_;
            const auto sourceSky = SourceSkyModel(entitiesSource_);
            if (sourceSky && o_.bo2 && o_.techsetDump)
            {
                const auto sky = Call(StageSourceSkybox, Json{{"project", o_.project}, {"project_root", Str(projectRoot_)}, {"roots", Paths(roots_)},
                                                              {"source_name", *sourceSky}, {"native_root", Str(*o_.techsetDump)}, {"bo2_root", Str(*o_.bo2)}});
                RaiseReported(sky);
                Extend(report_, "materials", sky["materials"]);
                report_["content"]["sky"] = sky["sky"];
                const auto skyMaterials = NameSet(sky["sky"]["materials"]);
                for (const auto& material : report_["materials"])
                    if (skyMaterials.contains(material["name"].get<std::string>()))
                    {
                        skyTechset = material["t6_techset"].get<std::string>();
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
                const auto& content = report_["content"];
                const auto skyMaterials = content.contains("sky") ? NameSet(content["sky"].value("materials", Json::array())) : Names{};
                for (const auto& material : report_["materials"])
                    if (skyMaterials.contains(material["name"].get<std::string>()))
                        used.insert(material["t6_techset"].get<std::string>());
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

        Json BridgeStage::Tail(Json scripts, const std::vector<std::string>& images, const Json& hudMaterials, const Names& zoneModels)
        {
            const auto wawRoots = MapScriptFolder();
            Json nativeFallbacks = Json::object();
            for (const auto& [name, path] : nativeFallbacks_)
                nativeFallbacks[name] = Str(path);
            Names convertedFx;
            for (const auto& [name, _] : effects_.table.items())
                convertedFx.insert(name);
            std::vector<fs::path> rawRoots;
            if (o_.wawRoot)
                rawRoots.push_back(*o_.wawRoot / "raw");
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
            const Json request{
                {"scripts", std::move(scripts)},
                {"report", report_},
                {"project", o_.project},
                {"project_root", Str(projectRoot_)},
                {"stage", Str(o_.stage)},
                {"roots", Paths(roots_)},
                {"bo2_root", PathOrNull(o_.bo2)},
                {"techset_dump", PathOrNull(o_.techsetDump)},
                {"t4_unlinker", PathOrNull(o_.t4Unlinker)},
                {"t6_unlinker", PathOrNull(o_.t6Unlinker)},
                {"waw_map_stem", o_.wawMapScript ? Json(files::Utf8(o_.wawMapScript->stem())) : Json()},
                {"raw_roots", Paths(rawRoots)},
                {"iwd_dirs", Paths(o_.iwdDirs.value_or(std::vector<fs::path>{}))},
                {"stock_waw", bridge::StockRequest(stock_ ? &*stock_ : nullptr)},
                {"images", images},
                {"hud_materials", hudMaterials},
                {"zone_models", SortedNames(zoneModels)},
                {"zone_fx", effects_.zoneFx},
                {"ipak", !o_.noIpak},
                {"bo2_stock_perks", o_.bo2StockPerks},
                {"weapons", Json{{"staged", weaponReport_.has_value()}, {"stock_materials", Str(o_.stockMaterials)}, {"native_fallbacks", nativeFallbacks},
                                 {"layered_vertex_data", layeredVertexData_}, {"converted_fx", SortedNames(convertedFx)},
                                 {"table_roots", Paths(Concat({o_.wawPatchScriptRoots, roots_}))},
                                 {"map_table_roots", Paths(Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots}))}}},
                {"visuals", Json{{"iwd_dirs", Paths(o_.iwdDirs.value_or(std::vector<fs::path>{}))},
                                 {"vision_roots", Paths(Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots, roots_, Optional(o_.wawStockScripts)}))}}},
                {"client", Json{{"has_water", hasWater_}}},
                {"sounds", Json{{"map_stem", o_.wawMapScript ? Json(files::Utf8(o_.wawMapScript->stem())) : Json()},
                                {"amb_roots", o_.wawMapScript ? Paths(Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots, roots_})) : Json::array()},
                                {"ents", Str(entitiesSource_)},
                                {"zone_sounds", SortedNames(zoneSounds_)},
                                {"compiled_sounds", SortedNames(compiledSounds_)},
                                {"iwd_dirs", Paths(imageIwds_)},
                                {"audio_decoder", PathOrNull(audioDecoder)},
                                {"xwma_helper", PathOrNull(xwma)},
                                {"resampler", resampler.empty() ? Json() : Json(Str(resampler))},
                                {"t6_sound_driver", PathOrNull(soundDriver)},
                                {"approximate_sound_curves", o_.approximateSoundCurves},
                                {"vision_report", nullptr}}},
                {"lighting", Json{{"base", Str(Sibling(o_.gfx, ".gfx.bin", ""))}, {"clip_source", Str(o_.clip)}, {"shadow_lights", shadowLights_},
                                  {"script_render_dvars", Json::object()}}},
                {"coverage_roots", Paths(Concat({o_.wawPatchScriptRoots, wawRoots, o_.wawScriptRoots, Optional(o_.wawStockScripts)}))}};
            auto tail = BridgeTail(request, *recovery_);
            bridge::AdoptPrepared(stock_ ? &*stock_ : nullptr, tail);
            return std::move(tail["report"]);
        }

        Json BridgeStage::Run()
        {
            OpenSources();
            StageGeometry();
            if (!o_.wawMapScript || !o_.bo2)
                throw StepError("StageError", "the native bridge stage needs --waw-map-script and --bo2");
            PortScripts();
            const auto listened = NameSet(Script(report_, "client_listener_strings"));
            const auto entitiesJson = projectRoot_ / "BSP/entities.json";
            if (!listened.empty() && mod::IsFile(entitiesJson))
                if (const auto networked = NetworkClientModels(entitiesJson, listened); !networked.empty())
                    report_["warnings"].push_back("CLIENT_MODELS_NETWORKED " + mod::Join(networked, ", ") +
                                                  ": client-side script_models driven by WaW client listeners, which the port runs on the server");
            std::vector<std::string> fxNames;
            for (const auto& name : Script(report_, "fx"))
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
                report_["warnings"].push_back("no --waw-root/--t4-unlinker: assets the map only references cannot be looked up in the stock WaW zones");
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
                worldModels.insert(model[0].get<std::string>());
            // entity placements need the same complete lookup as script-only models
            resolved_ = bridge::RecoverScriptModels(report_, Union(Union(Union(scriptModels_, shipped_), clipModels_), worldModels), roots_, stock);
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
                    report_["warnings"].push_back("ENTITY_MODEL_MISSING " + mod::ListRepr(std::vector<std::string>(absent.begin(), absent.end())) +
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
            for (const auto& material : report_["materials"])
                if (material.contains("shader_runtime") && material["shader_runtime"].is_object() &&
                    material["shader_runtime"].value("lightmap", Json()) == "waw")
                    wawLightmapMaterials.insert(material.value("source", material["name"]).get<std::string>());
            StageLightmaps(SortedNames(wawLightmapMaterials));
            const auto images = StageSkyAndImages(used);

            Json scripts{{"used_techsets", SortedNames(used)},
                         {"remaster_roots", Json::array()},
                         {"waw_map_script", Str(*o_.wawMapScript)},
                         {"required_template", o_.scriptTemplate && o_.scriptTemplateName
                                                   ? Json{{"root", Str(*o_.scriptTemplate)}, {"name", *o_.scriptTemplateName}}
                                                   : Json()},
                         {"drop_zone_manager", zoneGraph_.is_null() ? Json() : Json(true)},
                         {"zone_graph", zoneGraph_.is_null() ? Json() : Json{{"initial", zoneGraph_["initial"]}, {"adjacency", zoneGraph_["adjacency"]}}}};
            const auto zoneModels = Union(Union(Union(scriptModels_, Intersection(shipped_, resolved_)), fxModels), projectileModels);
            return Tail(std::move(scripts), images, hudMaterials, zoneModels);
        }

        // cli._stage_bridge's summary
        int PrintSummary(const Json& report, const std::string& project)
        {
            std::printf("collision: %s\n", py::JsonDumpsDefault(report["collision"]).c_str());
            std::size_t degraded = 0;
            for (const auto& material : report["materials"])
                if (material.contains("notes") && Truthy(material["notes"]))
                    ++degraded;
            std::printf("materials: %zu written, %zu with degradations (see zone_raw/%s/bridge_stage.report.json)\n", report["materials"].size(), degraded,
                        project.c_str());
            std::printf("images: %zu IWI written; technique sets used: %zu\n", report["images"].size(), report["techsets"].size());
            for (const auto& warning : report["warnings"])
                std::printf("warning: %s\n", warning.get<std::string>().c_str());
            std::fflush(stdout);
            const auto& errors = report["errors"];
            for (std::size_t i = 0; i < errors.size() && i < 40; ++i)
                std::fprintf(stderr, "error: %s\n", errors[i].get<std::string>().c_str());
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
                                                 {"--fx-fallback", &o.fxFallback},
                                                 {"--waw-source-fx", &o.wawSourceFx},
                                                 {"--approximate-sound-curves", &o.approximateSoundCurves},
                                                 {"--bo2-stock-perks", &o.bo2StockPerks},
                                                 {"--waw-stock-map", &o.wawStockMap},
                                                 {"--remaster-bo2-materials", &o.remasterBo2Materials}};
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

    std::string BridgeStageOptions::PythonOnlyOption() const
    {
        if (wawSourceFx)
            return "--waw-source-fx";
        if (fxFallback)
            return "--fx-fallback";
        if (remasterBo2Materials)
            return "--remaster-bo2-materials";
        return {};
    }

    int StageBridge(const BridgeStageOptions& options)
    {
        if (const auto option = options.PythonOnlyOption(); !option.empty())
            throw StepError("NotImplementedError", option + " is staged by the Python conversion worker");
        const timeline::Span span("cli stage-bridge");
        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        BridgeStage stage(options);
        const auto report = stage.Run();
        return PrintSummary(report, options.project);
    }
} // namespace w2b::pipeline
