// The bridge stage's WaW asset passes (see bridge_assets.h).
#include "bridge_assets.h"

#include "assets/resolver.h"
#include "bridge_images.h"
#include "bridge_support.h"
#include "core/asset_paths.h"
#include "core/regex.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "extract/wawsource.h"
#include "finish.h"
#include "materials/donor_index.h"
#include "materials_stage.h"
#include "textures/dds.h"
#include "weapons/stage.h"

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace w2b::pipeline::bridge
{
    namespace
    {
        Json SortedNames(const Names& names)
        {
            return Json(std::vector<std::string>(names.begin(), names.end()));
        }

        Names NameSet(const Json& list)
        {
            Names names;
            if (list.is_array())
                for (const auto& name : list)
                    names.insert(name.get<std::string>());
            return names;
        }

        std::optional<fs::path> Find(const std::vector<fs::path>& roots, const fs::path& relative)
        {
            std::error_code e;
            for (const auto& root : roots)
                if (fs::exists(root / relative, e))
                    return root / relative;
            return std::nullopt;
        }

        int ZoneRank(const std::string& zone)
        {
            return zone.starts_with("nazi_zombie") ? 0 : zone.starts_with("localized") ? 2 : 1;
        }

        // assetresolve.Resolver.expand over the stock WaW zones
        assets::AssetGraph Expand(const std::vector<fs::path>& roots, extract::StockWaw* stock, const assets::AssetSet& requests)
        {
            assets::StockLookup lookup;
            assets::StockSounds sounds;
            if (stock)
            {
                lookup = [stock](const assets::Asset& asset) { return stock->RootFor(asset.first, asset.second); };
                sounds = [stock]
                {
                    std::set<std::string> names;
                    for (const auto& [_, kinds] : stock->Index().items())
                        if (kinds.contains("sound"))
                            for (const auto& name : kinds["sound"])
                                names.insert(name.get<std::string>());
                    return names;
                };
            }
            try
            {
                return assets::Expand(extract::ResourceRoot(), roots, requests, std::move(lookup), std::move(sounds));
            }
            catch (const weapons::WeaponError& error)
            {
                throw StepError("WeaponError", error.what());
            }
        }

        // WawSource::Compile; a failed compile fails the stage
        std::optional<fs::path> CompileSource(extract::WawSource& source, const std::string& kind, const std::string& name)
        {
            try
            {
                return source.Compile(kind, name);
            }
            catch (const extract::WawSourceError& error)
            {
                throw StepError("WawSourceError", error.what());
            }
        }


        // sorted(dict.items())
        std::map<std::string, Json> SortedItems(const Json& object)
        {
            std::map<std::string, Json> items;
            for (const auto& [key, value] : object.items())
                items.emplace(key, value);
            return items;
        }

        std::string ListText(const Json& items)
        {
            std::vector<std::string> values;
            for (const auto& item : items)
                values.push_back(item.is_string() ? item.get<std::string>() : item.dump());
            return mod::ListRepr(values);
        }

        // weapons.stage: compiled weapon fields, animation tracks and models
        Json StageWeaponAssets(std::vector<fs::path> roots, const fs::path& content, const Names& weapons, const Names& scriptModels,
                               extract::StockWaw* stock)
        {
            std::optional<assets::AssetGraph> graph;
            if (stock)
            {
                // only map and companion weapons are initial roots: a stock dump
                // made for one dependency can also contain unrelated guns
                assets::AssetSet requests;
                for (const auto& name : weapons)
                    requests.emplace("weapon", name);
                for (const auto& name : scriptModels)
                    requests.emplace("xmodel", name);
                graph = Expand(roots, stock, requests);
                roots = graph->roots;
            }
            auto staged = Reported([&] { return weapons::StageWeapons(roots, content, extract::ResourceRoot(), {weapons.begin(), weapons.end()}); });
            auto& report = staged.report;
            const auto models = report["dependencies"].value("xmodel", Json::array()).get<weapons::Names>();
            report["models"] = Reported([&] { return weapons::StageModels(roots, content, models, staged.worldModels); });
            if (graph)
            {
                report["dependency_graph"] = "weapons.dependencies.json";
                report["resolved_roots"] = graph->report["roots"];
                files::WriteText(content / "weapons.dependencies.json", util::JsonDumps(graph->report, 2) + "\n");
            }
            files::WriteText(content / "weapons.stage.json", util::JsonDumps(report, 2) + "\n");
            return report;
        }

        // material_donors: techniqueSet -> stock donor material whose technique set was dumped
        Json Donors(const fs::path& stockMaterials, const std::optional<fs::path>& techsetRoot)
        {
            auto donors = materials::IndexDonors(stockMaterials);
            if (!techsetRoot)
                return donors;
            Json usable = Json::object();
            for (const auto& [techset, path] : donors.items())
                if (mod::IsFile(*techsetRoot / "techniquesets" / files::FromUtf8(techset + ".json")))
                    usable[techset] = path;
            return usable;
        }
    } // namespace

    unsigned WorkerCount(const std::size_t total, const bool processes)
    {
        unsigned workers = std::max(1u, std::thread::hardware_concurrency());
        if (const char* configured = std::getenv("WAW2BO2_WORKERS"); configured && *configured)
        {
            const int value = std::atoi(configured);
            if (value < 1)
                throw StepError("ValueError", "WAW2BO2_WORKERS must be a positive integer");
            workers = static_cast<unsigned>(value);
        }
        if (processes)
            workers = std::min(workers, 61u); // ProcessPoolExecutor's Windows handle limit
        return static_cast<unsigned>(std::min<std::size_t>(workers, std::max<std::size_t>(1, total)));
    }

    Names EngineZoneWeapons(BridgeReport& report, std::vector<fs::path>& roots, const fs::path& projectRoot, const Names& compiled, extract::StockWaw& stock)
    {
        // later zones override earlier ones: the newest first
        static const std::vector<std::string> zones{"patch", "common", "code_post_gfx"};
        static const regex::Regex call("precacheitem\\s*\\(\\s*\"([^\"\\n]+)\"", regex::IGNORECASE);
        Names named;
        for (const auto& file : files::GlobSuffix(projectRoot / "maps/mp/waw", ".gsc"))
            for (const auto& row : call.FindAll(files::ReadText(file)))
                named.insert(mod::Lower(row[0]));
        Names known;
        for (const auto& name : compiled)
            known.insert(mod::Lower(name));
        Names out;
        std::vector<std::string> sources;
        for (const auto& name : named)
        {
            if (known.contains(name))
                continue;
            const auto defining = stock.ZonesDefining("weapon", name);
            const auto zone = std::ranges::find_if(zones, [&](const std::string& z) { return std::ranges::find(defining, z) != defining.end(); });
            if (zone == zones.end())
                continue;
            const auto root = stock.Dump(*zone, "weapon,xanim,xmodel");
            if (std::ranges::find(roots, root) == roots.end())
                roots.push_back(root);
            out.insert(name);
            sources.push_back(name + " (" + *zone + ".ff)");
        }
        if (!sources.empty())
            report.Warn("ENGINE_ZONE_WEAPONS " + mod::Join(sources, ", ") + ": precached by the map's scripts, from the zones WaW always loads");
        return out;
    }

    std::optional<Json> StageWeapons(BridgeReport& report, std::vector<fs::path>& roots, const fs::path& projectRoot, const Names& weapons,
                                     const Names& scriptModels, extract::StockWaw* stock, Names& sounds)
    {
        if (weapons.empty() && scriptModels.empty())
            return std::nullopt;
        return Timed("Resolve weapons and dependencies", [&]
        {
            // the scripts' models only widen the resolved roots (stock WaW
            // dumps); stage_models stages them into the map zone under their WaW names
            auto weaponReport = StageWeaponAssets(roots, projectRoot / "content_source", weapons, scriptModels, stock);
            if (weaponReport.contains("resolved_roots"))
                roots = PathList(weaponReport["resolved_roots"]);
            for (const auto& name : weaponReport["dependencies"].value("sound", Json::array()))
                sounds.insert(name.get<std::string>());
            report.content["weapons"] = Json{{"status", weaponReport["status"]}, {"weapons", weaponReport["weapons"].size()},
                                                {"animations", weaponReport["animations"].size()},
                                                {"missing_animations", weaponReport["missing_animations"]},
                                                {"report", "content_source/weapons.stage.json"}};
            const auto& models = weaponReport["models"];
            for (const auto& [name, bones] : SortedItems(models.value("reparented_roots", Json::object())))
                report.Warn("SKELETON_REPARENTED xmodel " + name + ": root bones " + ListText(bones) +
                                             " share one bind transform; secondary roots became identity children of the primary (globals unchanged)");
            for (const auto& name : models.value("rebound_to_root", Json::array()))
                report.Warn("WORLD_MODEL_REBOUND xmodel " + name.get<std::string>() +
                                             ": over the mod tools linker's animated vertex limit; weights moved to the root bone, all bones kept (world models do not animate)");
            for (const auto& [name, why] : SortedItems(models.at("unsupported")))
                report.Warn("UNSUPPORTED_XMODEL " + name + " (weapon model): " + why.get<std::string>());
            const auto graphFile = projectRoot / "content_source/weapons.dependencies.json";
            const auto graph = mod::IsFile(graphFile) ? mod::Json(graphFile) : Json::object();
            if (graph.contains("nodes"))
                for (const auto& node : graph["nodes"])
                {
                    for (const auto& source : node.value("recovered_geometry", Json::array()))
                        report.Warn("WAW_MODEL_GEOMETRY_RECOVERED " + node["name"].get<std::string>() + ": " + source.get<std::string>());
                    if (node["status"] == "missing_model_geometry")
                    {
                        std::vector<std::string> missing;
                        for (const auto& file : node["missing_files"])
                            missing.push_back(file.get<std::string>());
                        report.Fail("MODEL_GEOMETRY_MISSING " + node["name"].get<std::string>() + ": the map and WaW stock assets do not contain " +
                                                   mod::Join(missing, ", ") + "; verify the source map and WaW installation files");
                    }
                }
            return weaponReport;
        });
    }

    Names RecoverScriptModels(Json& scripts, std::vector<std::string>& warnings, const Names& wanted, std::vector<fs::path>& roots, extract::StockWaw* stock,
                              extract::WawSource* source)
    {
        // a setModel-only asset need not occur in the map's entity/model table:
        // those requests go through the complete WaW lookup, without BO2 substitution
        const auto sourceRoots = roots;
        std::vector<std::pair<std::string, fs::path>> stockRoots;
        std::vector<fs::path> compiledRoots;
        const auto find = [&](const std::string& name) -> std::optional<fs::path>
        {
            const auto relative = assets::SourcePath({"xmodel", name});
            for (const auto& root : sourceRoots)
                if (mod::IsFile(root / relative))
                    return root / relative;
            if (!stock)
                return std::nullopt;
            const auto hit = stock->RootFor("xmodel", name);
            if (!hit)
                return std::nullopt;
            const auto known = std::ranges::find(stockRoots, hit->first, &std::pair<std::string, fs::path>::first);
            if (known == stockRoots.end())
                stockRoots.push_back(*hit);
            else
                known->second = hit->second;
            if (mod::IsFile(hit->second / relative))
                return hit->second / relative;
            return std::nullopt;
        };
        Names models;
        Json dependencies = Json::array();
        for (const auto& name : wanted)
        {
            auto path = find(name);
            std::string provenance = "compiled_waw";
            if (!path && source)
                if (const auto recovered = CompileSource(*source, "xmodel", name))
                {
                    compiledRoots.push_back(*recovered);
                    path = *recovered / assets::SourcePath({"xmodel", name});
                    provenance = "WAW_SOURCE_ASSET";
                    warnings.push_back("WAW_SOURCE_ASSET xmodel " + name + ": compiled from WaW Mod Tools");
                }
            Json node{{"name", name}, {"status", path ? "resolved" : "missing_waw_source"}};
            if (path)
            {
                models.insert(name);
                node["source"] = Str(*path);
                node["provenance"] = provenance;
            }
            dependencies.push_back(std::move(node));
        }
        std::ranges::stable_sort(stockRoots, {}, [](const auto& hit) { return std::pair{ZoneRank(hit.first), std::string_view(hit.first)}; });
        std::vector<fs::path> widened;
        std::set<std::string> seen;
        const auto add = [&](const fs::path& root)
        {
            if (seen.insert(Str(root)).second)
                widened.push_back(root);
        };
        for (const auto& root : sourceRoots)
            add(root);
        for (const auto& [_, root] : stockRoots)
            add(root);
        for (const auto& root : compiledRoots)
            add(root);
        roots = std::move(widened);
        scripts["requested_models"] = SortedNames(wanted);
        scripts["model_sources"] = std::move(dependencies);
        return models;
    }

    std::map<std::string, fs::path> RecoverMaterialSources(BridgeReport& report, const Names& names, std::vector<fs::path>& roots, extract::StockWaw* stock,
                                                           extract::WawSource* source, const fs::path& stockMaterials, const std::optional<fs::path>& bo2Root)
    {
        // compiled WaW definitions win; BO2 equivalents must be explicit or
        // exact-name matches, and keep their native image payloads
        assets::AssetSet requests;
        for (const auto& name : names)
            requests.emplace("material", name);
        auto graph = Expand(roots, stock, requests);
        roots = std::move(graph.roots);
        auto& closure = graph.report;
        const auto equivalents = mod::Json(extract::ResourceRoot() / "src/waw2bo2/compat/bo2_equivalents.json").value("material", Json::object());
        std::map<std::string, std::size_t> nodeIndex;
        for (std::size_t i = 0; i < closure["nodes"].size(); ++i)
            if (closure["nodes"][i]["kind"] == "material")
                nodeIndex.emplace(closure["nodes"][i]["name"].get<std::string>(), i);
        std::vector<fs::path> candidates{stockMaterials.parent_path()};
        if (bo2Root)
            candidates.push_back(*bo2Root / "raw");
        std::map<std::string, fs::path> fallbacks;
        for (const auto& name : names)
        {
            if (Find(roots, assets::MaterialPath(name)))
                continue;
            if (source)
                if (const auto recovered = CompileSource(*source, "material", name))
                {
                    roots.push_back(*recovered);
                    report.Warn("WAW_SOURCE_ASSET material " + name + ": compiled from WaW Mod Tools");
                    auto& node = closure["nodes"][nodeIndex.at(name)];
                    node["status"] = "resolved";
                    node["provenance"] = "WAW_SOURCE_ASSET";
                    node["source"] = Str(*recovered / assets::MaterialPath(name));
                    continue;
                }
            // "*a_b_c" names are generated per compiled map from its own blend
            // components; BO2's generated/_a_b_c is another map's blend that
            // only shares the numbering
            if (name.starts_with('*') && !equivalents.contains(name))
                continue;
            const auto target = equivalents.contains(name) ? equivalents[name].get<std::string>() : name;
            if (const auto candidate = Find(candidates, assets::MaterialPath(target)))
            {
                fallbacks.emplace(name, *candidate);
                closure["nodes"][nodeIndex.at(name)]["native_equivalent"] = Str(*candidate);
            }
        }
        closure["roots"] = Paths(roots);
        Json missing = Json::array();
        for (const auto& node : closure["nodes"])
            if (node["status"] != "resolved")
                missing.push_back(node);
        closure["missing"] = std::move(missing);
        report.content["material_dependencies"] = std::move(closure);
        return fallbacks;
    }

    Names StageMaterials(BridgeReport& report, const Names& names, const std::vector<fs::path>& roots, const fs::path& stockMaterials,
                         const fs::path& projectRoot, const std::optional<fs::path>& techsetRoot, const MaterialOptions& options)
    {
        return Timed("Convert materials", [&]
        {
            const auto donors = Donors(stockMaterials, techsetRoot);
            if (donors.empty())
                throw StepError("StageError", "no stock T6 material donors found under " + Str(stockMaterials));
            MaterialStageRequest request{{names.begin(), names.end()}, roots, stockMaterials, projectRoot, techsetRoot, "waw_world/", donors,
                                         options.falloffPlacement, options.lightShadows, options.layeredVertexData, options.nativeFallbacks};
            Names used;
            for (auto& result : pipeline::StageMaterials(request))
            {
                Extend(report, "images", result.images);
                Extend(report, "warnings", result.warnings);
                if (!result.native)
                    Extend(report, "errors", result.errors);
                if (result.entry)
                {
                    report.materials.push_back(std::move(*result.entry));
                    used.insert(result.techset);
                }
            }
            return used;
        });
    }

    std::vector<std::string> StageImages(BridgeReport& report, const std::vector<fs::path>& imageRoots, const fs::path& projectRoot,
                                         const std::optional<std::vector<fs::path>>& iwdDirs, const std::vector<std::string>& extraImages,
                                         textures::IwdRecovery& recovery, Json& recovered)
    {
        return Timed("Convert images", [&]
        {
            std::vector<std::string> materials;
            std::set<std::string> nativeImages;
            for (const auto& material : report.materials)
                materials.push_back(material.file);
            for (const auto& image : report.images)
                if (image.value("source_kind", Json()) == "t6_equivalent")
                    nativeImages.insert(image["name"].get<std::string>());
            const auto flag = [](const char* name)
            {
                const char* value = std::getenv(name);
                return value && *value;
            };
            const BridgeImagesRequest request{projectRoot, imageRoots, iwdDirs, extraImages, materials, nativeImages, recovered,
                                              flag("WAW2BO2_DIAG_FALLBACK_MAGENTA"), flag("WAW2BO2_DIAG_PROBES"), WorkerCount(0x7FFFFFFF)};
            auto result = StageBridgeImages(request, recovery);
            for (const auto& [stem, metadata] : result.recovered.items())
                recovered[stem] = metadata;
            Extend(report, "images", result.images);
            for (auto& warning : result.warnings)
                report.Warn(std::move(warning));
            for (auto& error : result.errors)
                report.Fail(std::move(error));
            return std::move(result.written);
        });
    }

    Json StageImpactMaterials(const fs::path& projectRoot, const std::vector<fs::path>& roots, const fs::path& techsetDump,
                              extract::StockWaw* stock, textures::IwdRecovery& recovery, Json& recovered, const bool layeredVertexData)
    {
        Names names;
        for (const auto& root : roots)
            for (const char* family : {"mc", "wc"})
                for (const auto& file : files::GlobSuffix(root / "materials" / family, ".json"))
                {
                    const auto stem = files::Utf8(file.stem());
                    if (stem.starts_with("gfx_impact_"))
                        names.insert(std::string(family) + "/" + stem);
                }
        if (names.empty())
            return Json::object();
        assets::AssetSet requests;
        for (const auto& name : names)
            requests.emplace("material", name);
        const auto graph = Expand(roots, stock, requests);
        const auto& resolvedRoots = graph.roots;
        const auto& closure = graph.report;
        const auto output = projectRoot / "content_source/impact_materials";
        fs::create_directories(output);
        BridgeReport report;
        report.project = "source_impact_materials";
        MaterialOptions options;
        options.layeredVertexData = layeredVertexData;
        StageMaterials(report, names, resolvedRoots, techsetDump / "materials", output, techsetDump, options);
        std::vector<fs::path> imageRoots;
        for (const auto& root : resolvedRoots)
            imageRoots.push_back(root / "images");
        StageImages(report, imageRoots, output, std::nullopt, {}, recovery, recovered);
        if (!report.errors.empty())
        {
            const std::vector<std::string> first(report.errors.begin(), report.errors.begin() + static_cast<long long>(std::min<std::size_t>(8, report.errors.size())));
            throw StepError("ValueError", "source impact materials: " + mod::Join(first, "; "));
        }
        files::WriteText(output / "stage.json", util::JsonDumps(Json{{"materials", ToJson(report.materials)}, {"images", report.images}}, 1));
        return Json{{"materials", report.materials.size()}, {"report", "content_source/impact_materials/stage.json"},
                    {"missing_dependencies", closure["missing"]}};
    }

    std::optional<std::array<double, 4>> LightFalloffPlacement(const fs::path& primaryLights, const std::vector<fs::path>& roots)
    {
        // WaW bakes each light def's attenuation ramp into row 0 of every
        // secondary lightmap page from lmapLookupStart and samples it at v = 0
        try
        {
            std::set<std::string> defs;
            const auto table = mod::Json(primaryLights);
            for (const auto& light : table.at("lights"))
                if (light.value("type", 0) > 1 && light.contains("defName") && Truthy(light["defName"]))
                    defs.insert(light["defName"].get<std::string>());
            if (defs.size() != 1)
                return std::nullopt;
            const auto definitionFile = Find(roots, files::FromUtf8("lightdef/" + *defs.begin() + ".json"));
            if (!definitionFile)
                return std::nullopt;
            const auto definition = mod::Json(*definitionFile);
            auto attenuationName = definition.at("attenuation").get<std::string>();
            if (attenuationName.starts_with(','))
                attenuationName.erase(0, 1);
            const auto attenuation = Find(roots, files::FromUtf8("images/" + attenuationName + ".dds"));
            const auto page = Find(roots, "images/_lightmap0_secondary.dds");
            if (!attenuation || !page)
                return std::nullopt;
            const auto width = textures::ReadDds(mod::Bytes(*attenuation)).width;
            const auto pageWidth = textures::ReadDds(mod::Bytes(*page)).width;
            return std::array<double, 4>{width / 512.0, 0.0, definition.at("lmapLookupStart").get<double>() / 512.0, 0.5 / (2.0 * pageWidth)};
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    void WriteImageStreaming(const fs::path& projectRoot, const Names& images, const bool merge)
    {
        constexpr int EFFECT_IMAGE_STREAMING = 2;
        const auto path = projectRoot / "images/streaming.json";
        std::map<std::string, Json> modes;
        if (merge && mod::IsFile(path))
        {
            const auto existing = mod::Json(path);
            for (const auto& [name, mode] : existing.at("streamingMode").items())
                modes[name] = mode;
        }
        for (const auto& name : images)
            modes[name] = EFFECT_IMAGE_STREAMING;
        Json sorted = Json::object();
        for (const auto& [name, mode] : modes)
            sorted[name] = mode;
        files::WriteText(path, util::JsonDumps(Json{{"streamingMode", sorted}}, 2) + "\n");
    }
} // namespace w2b::pipeline::bridge
