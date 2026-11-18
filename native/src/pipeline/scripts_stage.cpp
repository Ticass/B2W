// t6bridge.port_scripts: the WaW map's gameplay and client scripts, their
// animation dependencies, script models and HUD materials (see scripts_stage.h).
#include "scripts_stage.h"

#include "assets/resolver.h"
#include "barriers/barriers.h"
#include "core/asset_paths.h"
#include "core/pyre.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "gsc/port/api.h"
#include "gsc/port/clientport.h"
#include "gsc/port/generators.h"
#include "gsc/port/portmap.h"
#include "gsc/port/rewrites.h"
#include "gsc/port/support.h"
#include "gsc/port/zombiebehavior.h"
#include "gsc/tokens.h"
#include "modutil.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::Bytes;
    using mod::Copy;
    using mod::Generic;
    using mod::IsDir;
    using mod::IsFile;
    using mod::Lower;
    using mod::StartsWith;
    using mod::Str;
    using py::Json;
    namespace port = gsc::port;

    namespace
    {
        const std::string CORE_ANIMTREE_HEADER = "// waw2bo2: WaW animtree";
        const std::vector<std::string> AMBIENT_AND_MUSIC_CALLS{"declareambientpackage", "addambientelement", "declareambientroom", "setambientroomreverb",
                                                               "setambientroomtone", "activateambientpackage", "activateambientroom", "declaremusicstate",
                                                               "musicalias", "musicaliasloop", "musicwaittilldone", "musicwaittillstingerdone", "musicstinger"};
        const std::vector<std::string> REPLACE_CHUNK_SCRIPTS{"maps\\_zombiemode_blockers_new", "maps\\_zombiemode_blockers"};
        const std::map<std::string, std::string> ENGINE_BUILTIN_MATERIALS{{"setelectrified", "zombie_electric_shock_overlay"}};

        Json ReadJson(const fs::path& p)
        {
            return mod::Json(p);
        }

        std::vector<fs::path> Paths(const Json& values)
        {
            std::vector<fs::path> out;
            for (const auto& v : values)
                out.push_back(files::FromUtf8(v.get<std::string>()));
            return out;
        }

        int ZoneRank(const std::string& zone)
        {
            return StartsWith(zone, "nazi_zombie") ? 0 : StartsWith(zone, "localized") ? 2 : 1;
        }

        // assetresolve.Resolver: find and roots (source roots, then the stock dumps used)
        class Resolver
        {
        public:
            Resolver(std::vector<fs::path> roots, extract::StockWaw* stock) : sourceRoots(std::move(roots)), stock_(stock)
            {
            }

            std::optional<fs::path> Find(const std::string& kind, const std::string& name)
            {
                const auto relative = assets::SourcePath({kind, name});
                const bool loaded = kind == "loadedsound";
                for (const auto& root : sourceRoots)
                    if (auto p = assets::FindFile(root, relative, loaded); !p.empty())
                        return p;
                if (StartsWith(kind, "accuracy_"))
                {
                    for (const auto& [_, root] : stockRoots)
                        if (IsFile(root / relative))
                            return root / relative;
                    return std::nullopt;
                }
                if (!stock_)
                    return std::nullopt;
                const auto hit = stock_->RootFor(kind, name);
                if (!hit)
                    return std::nullopt;
                stockRoots[hit->first] = hit->second;
                if (auto p = assets::FindFile(hit->second, relative, loaded); !p.empty())
                    return p;
                return std::nullopt;
            }

            std::vector<fs::path> Roots() const
            {
                auto out = sourceRoots;
                std::vector<std::string> zones;
                for (const auto& [z, _] : stockRoots)
                    zones.push_back(z);
                std::sort(zones.begin(), zones.end(), [](const std::string& a, const std::string& b) { return std::pair(ZoneRank(a), a) < std::pair(ZoneRank(b), b); });
                for (const auto& z : zones)
                    out.push_back(stockRoots.at(z));
                return out;
            }

            std::vector<fs::path> sourceRoots;
            std::map<std::string, fs::path> stockRoots;

        private:
            extract::StockWaw* stock_;
        };

        // list(dict.fromkeys(paths))
        std::vector<fs::path> Dedupe(const std::vector<fs::path>& paths)
        {
            std::vector<fs::path> out;
            std::set<std::string> seen;
            for (const auto& p : paths)
                if (seen.insert(Str(p)).second)
                    out.push_back(p);
            return out;
        }

        std::vector<std::string> FindAllGroup(const pyre::Regex& regex, const std::string& text)
        {
            std::vector<std::string> out;
            for (const auto& m : regex.FindAll(text))
                out.push_back(m[0]);
            return out;
        }

        // byte offset of a code point offset in UTF-8 text
        size_t ByteOffset(const std::string& text, const long long codePoint)
        {
            long long count = 0;
            for (size_t i = 0; i < text.size(); i++)
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                {
                    if (count == codePoint)
                        return i;
                    count++;
                }
            return text.size();
        }

        // wrapped_model_literals: models named through a helper that hands its parameter to a model call
        std::set<std::string> WrappedModelLiterals(const std::vector<std::string>& texts)
        {
            static const pyre::Regex function("^([A-Za-z_]\\w*)\\s*\\(([^)]*)\\)\\s*\\{", pyre::MULTILINE);
            std::map<std::string, size_t> wrappers;
            for (const auto& text : texts)
                for (const auto& m : function.FindIter(text))
                {
                    const auto start = ByteOffset(text, m.End(0));
                    int depth = 0;
                    size_t end = text.size();
                    for (size_t i = start - 1; i < text.size(); i++)
                    {
                        depth += text[i] == '{' ? 1 : text[i] == '}' ? -1 : 0;
                        if (depth == 0)
                        {
                            end = i;
                            break;
                        }
                    }
                    const auto body = text.substr(start, end - start);
                    size_t index = 0;
                    const auto params = m[2];
                    for (const auto part : py::Split(params, ","))
                    {
                        const auto param = std::string(py::Strip(part));
                        if (!param.empty() &&
                            pyre::Regex("\\b(?:waw_)?(?:precachemodel|setmodel|setviewmodel|attach)\\s*\\(\\s*" + pyre::Escape(param) + "\\s*[,)]", pyre::IGNORECASE)
                                .Search(body))
                            wrappers[Lower(m[1])] = index;
                        index++;
                    }
                }
            std::set<std::string> found;
            for (const auto& [name, index] : wrappers)
            {
                std::string pattern = "\\b" + pyre::Escape(name) + "\\s*\\(\\s*";
                for (size_t i = 0; i < index; i++)
                    pattern += "(?:\"[^\"]*\"\\s*,\\s*)";
                pattern += "\"([^\"]+)\"";
                const pyre::Regex call(pattern, pyre::IGNORECASE);
                for (const auto& text : texts)
                    for (const auto& model : FindAllGroup(call, text))
                        found.insert(model);
            }
            return found;
        }

        std::string SourceText(const port::Sources& sources, const std::string& key)
        {
            const auto* text = sources.Source(key);
            return text ? *text : std::string();
        }
    } // namespace

    Json PortScripts(const fs::path& requestPath)
    {
        const auto request = ReadJson(requestPath);
        if (!request["source_waw"].is_null() && request["source_waw"].get<bool>())
            throw StepError("NotImplementedError", "WaW Mod Tools sources are recovered by the Python step");
        const auto stage = files::FromUtf8(request["stage"].get<std::string>());
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        const auto mapScript = files::FromUtf8(request["waw_map_script"].get<std::string>());
        const auto bo2Root = files::FromUtf8(request["bo2_root"].get<std::string>());
        auto modelRoots = Paths(request["model_roots"]);
        const bool stockPerks = request.value("bo2_stock_perks", false), stockMap = request.value("waw_stock_map", false);
        auto stock = extract::StockWaw::FromJson(request.value("stock_waw", Json()));
        const auto mapName = files::Utf8(mapScript.stem());
        Json warnings = Json::array(), errors = Json::array(), scripts = Json::object();
        const auto finish = [&](const std::set<std::string>& models)
        {
            Json roots = Json::array();
            for (const auto& r : modelRoots)
                roots.push_back(Str(r));
            return Json{{"scripts", scripts}, {"warnings", warnings}, {"errors", errors}, {"models", std::vector<std::string>(models.begin(), models.end())},
                        {"model_roots", roots}, {"stock_prepared", stock ? stock->PreparedJson() : Json()}};
        };
        // the map zone's own rawfiles first; None marks the map zone's place
        std::vector<fs::path> scriptRoots;
        bool marked = false;
        for (const auto& r : request["roots"])
            marked |= r.is_null();
        if (!marked)
            scriptRoots.push_back(mapScript.parent_path().parent_path());
        for (const auto& r : request["roots"])
            scriptRoots.push_back(r.is_null() ? mapScript.parent_path().parent_path() : files::FromUtf8(r.get<std::string>()));
        std::vector<fs::path> iwds;
        {
            std::set<std::string> seen;
            for (const auto& d : Paths(request["iwd_dirs"]))
            {
                std::error_code e;
                if (!IsDir(d))
                    continue;
                for (const auto& entry : fs::directory_iterator(d, e))
                    if (Lower(files::Utf8(entry.path().extension())) == ".iwd" && seen.insert(Str(entry.path())).second)
                        iwds.push_back(entry.path());
            }
            std::sort(iwds.begin(), iwds.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
        }
        std::optional<fs::path> stockScripts;
        if (!request["stock"].is_null())
            stockScripts = files::FromUtf8(request["stock"].get<std::string>());
        std::error_code e;
        if (!stockScripts || !fs::exists(*stockScripts, e))
        {
            errors.push_back("no stock WaW scripts (--waw-stock-scripts): cannot tell the map's scripts from the WaW framework");
            return finish({});
        }
        port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        const auto api = port::T6Api::Load(extract::ResourceRoot() / "src/waw2bo2/compat/t6_script_api.json", stage / "t6api_cache.json");
        port::Sources sources(scriptRoots, iwds, stockScripts, Paths(request.value("stock_variant_roots", Json::array())));
        sources.BindZombieFramework("maps/" + mapName);
        const auto& definition = port::Api()["zombie_runtime"];
        const auto actorTypes = port::zombieappearance::SourceActorTypes(projectRoot);
        std::optional<port::ActorTemplates> actorTemplate;
        if (!actorTypes.empty())
        {
            port::ActorTemplates templates;
            for (const auto& [kind, cfg] : definition["actor_templates"].items())
            {
                const auto name = cfg.is_string() ? cfg.get<std::string>() : cfg["aitype"].get<std::string>();
                templates[kind] = files::ReadText(bo2Root / "raw/aitype" / files::FromUtf8(name + ".gsc"));
            }
            actorTemplate = templates;
        }
        // trees staged by an earlier run
        if (IsDir(projectRoot / "animtrees"))
            for (const auto& entry : fs::directory_iterator(projectRoot / "animtrees", e))
                if (Lower(files::Utf8(entry.path().extension())) == ".atr" && StartsWith(mod::ReadReplace(entry.path()), CORE_ANIMTREE_HEADER))
                    fs::remove(entry.path(), e);
        std::set<std::string> animtrees;
        for (const auto& dir : {bo2Root / "raw" / "animtrees", projectRoot / "animtrees"})
            if (IsDir(dir))
                for (const auto& entry : fs::directory_iterator(dir, e))
                    if (Lower(files::Utf8(entry.path().extension())) == ".atr")
                        animtrees.insert(files::Utf8(entry.path().stem()));
        // stage_core_animtrees: original animations reached by the script resolver
        std::map<std::string, std::set<std::string>> coreTrees;
        std::vector<std::string> xanims;
        {
            auto requirements = port::DiscoverAnimationDependencies(sources, api, mapName, actorTypes);
            const auto [movementTree, movement] = port::zombiebehavior::MovementSetsOf(sources, definition, mapName);
            if (movementTree && !movement.empty())
            {
                auto& names = requirements[*movementTree];
                for (const auto& [_, clips] : movement)
                    names.insert(clips.begin(), clips.end());
                for (const auto& [_, clips] : port::zombiebehavior::MeleeSets(sources, definition, mapName))
                    names.insert(clips.begin(), clips.end());
            }
            Resolver resolver(modelRoots, stock ? &*stock : nullptr);
            for (const auto& [tree, wanted] : requirements)
            {
                std::vector<std::string> found;
                for (const auto& name : wanted)
                {
                    const auto src = resolver.Find("xanim", name);
                    if (!src)
                    {
                        warnings.push_back("UNSUPPORTED_XANIM " + name + " (animtree " + tree + "): no WaW xanim in any zone dump; references become undefined");
                        continue;
                    }
                    const auto target = port::AnimationAssetName(name);
                    const auto dst = projectRoot / "xanim" / files::FromUtf8(target);
                    fs::create_directories(dst.parent_path());
                    Copy(*src, dst);
                    found.push_back(name);
                    xanims.push_back(target);
                }
                if (found.empty())
                    continue;
                const auto atr = projectRoot / "animtrees" / files::FromUtf8(port::AnimationTreeName(tree) + ".atr");
                fs::create_directories(atr.parent_path());
                std::string text = CORE_ANIMTREE_HEADER + " " + tree + ", animations with WaW xanims\n";
                for (const auto& n : found)
                    text += port::AnimationAssetName(n) + "\n";
                files::WriteText(atr, text);
                coreTrees[tree] = std::set<std::string>(found.begin(), found.end());
            }
            modelRoots = Dedupe(resolver.Roots());
        }
        // the map's own client scripts (stale ports of an earlier run removed first)
        const auto clientDir = projectRoot / "clientscripts/mp/waw";
        for (const auto& old : files::RGlob(clientDir, ".csc"))
            if (StartsWith(mod::ReadReplace(old), port::clientport::PORT_HEADER))
                fs::remove(old, e);
        std::set<std::string> builtins;
        for (const auto& [k, _] : api.builtins)
            builtins.insert(k);
        for (const auto& [k, _] : api.methods)
            builtins.insert(k);
        const auto client = port::clientport::PortMapClient(sources, mapName, projectRoot, port::clientport::StagedClientModules(mapName, files::Utf8(projectRoot.filename())),
                                                            builtins, {{"clientscripts\\" + Lower(mapName) + "_amb", std::set<std::string>(AMBIENT_AND_MUSIC_CALLS.begin(), AMBIENT_AND_MUSIC_CALLS.end())}});
        port::PortMapOptions options;
        options.animtrees = animtrees;
        options.coreAnimtrees = coreTrees;
        options.bo2StockPerks = stockPerks;
        options.actorTypes = actorTypes;
        options.actorTemplate = actorTemplate;
        options.stockMap = stockMap;
        options.clientPorted = mod::Truthy(Json{{"c", client}}, "c");
        // port_map rebuilds its source view from the arguments the sources were made with
        port::Sources fresh(scriptRoots, iwds, stockScripts, Paths(request.value("stock_variant_roots", Json::array())));
        fresh.BindZombieFramework("maps/" + mapName);
        const auto portReport = port::PortMap(fresh, api, mapName, projectRoot, options);
        if (!actorTypes.empty())
            port::zombieappearance::BindActorTypes(projectRoot, actorTypes);
        scripts = portReport.ToJson();
        if (mod::Truthy(Json{{"c", client}}, "c"))
        {
            scripts["client"] = client;
            std::set<std::string> fx;
            for (const auto& f : scripts.value("fx", Json::array()))
                fx.insert(f.get<std::string>());
            for (const auto& f : client["fx"])
                fx.insert(f.get<std::string>());
            scripts["fx"] = std::vector<std::string>(fx.begin(), fx.end());
            warnings.push_back("CLIENT_SCRIPTS the map's own client scripts run: " + std::to_string(client["modules"].size()) + " modules ported, entry " + client["entry"].get<std::string>());
            for (const auto& statement : client["dropped"])
                warnings.push_back("CLIENT_GLUE " + statement.get<std::string>() + ": started by the BO2 client main or its own port");
            for (const auto& name : client["compat"])
                warnings.push_back("CLIENT_COMPAT " + name.get<std::string>() + ": WaW client builtin T6 lacks or reads in other units; compat _waw2bo2_environment.csc");
            for (const auto& path : client["missing"])
                warnings.push_back("CLIENT_MISSING " + path.get<std::string>() + ": referenced by the map's client scripts, in no zone");
        }
        // the map's whole _zombiemode_score runs; no rules copied into BO2's scorer
        fs::remove(projectRoot / "maps/mp/waw/_waw2bo2_score_rules.gsc", e);
        if (!actorTypes.empty())
        {
            Resolver resolver(modelRoots, stock ? &*stock : nullptr);
            const port::zombiebehavior::FindXanim find = [&](const std::string& name) { return resolver.Find("xanim", name); };
            const auto movement = port::zombiebehavior::StageActorMovement(sources, projectRoot, bo2Root, definition, mapName, &find);
            if (mod::Truthy(Json{{"m", movement}}, "m"))
            {
                scripts["actor_movement"] = movement;
                for (const auto& c : movement.value("traversal_clips", Json::array()))
                    if (std::find(xanims.begin(), xanims.end(), c.get<std::string>()) == xanims.end())
                        xanims.push_back(c.get<std::string>());
                if (mod::Truthy(movement, "native_traversal_clips"))
                    warnings.push_back("BO2_FALLBACK traversal clips " + mod::Join(movement["native_traversal_clips"].get<std::vector<std::string>>(), ", ") +
                                       ": no WaW xanim of that name; BO2's clip stays");
                for (const auto& name : movement.value("missing", Json::array()))
                    warnings.push_back("SOURCE_MOVEMENT_XANIM " + name.get<std::string>() + ": no WaW xanim in any zone dump; left out of the zombie movement state");
                for (const auto& speed : movement.value("native_states", Json::array()))
                    warnings.push_back("BO2_FALLBACK movement " + speed.get<std::string>() + ": none of the WaW clips exist; the native BO2 clips stay");
                for (const auto& state : movement.value("native_melee_states", Json::array()))
                    warnings.push_back("BO2_FALLBACK melee " + state.get<std::string>() + ": a WaW attack clip is missing; the native BO2 attacks stay");
            }
        }
        scripts["staged_xanims"] = xanims;
        // the map's window rebuild motion (replace_chunk)
        for (const auto& script : REPLACE_CHUNK_SCRIPTS)
        {
            const auto text = SourceText(sources, port::Norm(script) + ".gsc");
            if (text.empty())
                continue;
            auto timing = barriers::RepairTiming(text);
            if (!timing.is_null())
            {
                Json result{{"script", script}};
                result.update(timing);
                scripts["window_repair_timing"] = result;
                break;
            }
        }
        {
            std::vector<std::string> trees;
            for (const auto& [t, _] : coreTrees)
                trees.push_back(port::AnimationTreeName(t));
            std::sort(trees.begin(), trees.end());
            scripts["staged_animtrees"] = trees;
        }
        for (const auto& x : portReport.errors)
            errors.push_back("scripts: " + x);
        files::WriteText(projectRoot / "maps/mp/waw/_waw2bo2_assets.gsc", port::AssetsSource({}, mapName));
        files::WriteText(projectRoot / "maps/mp/waw/_waw2bo2_oneway.gsc", port::oneway::OnewaySource());
        {
            std::vector<std::string> names(portReport.unsupported.order.begin(), portReport.unsupported.order.end());
            std::sort(names.begin(), names.end());
            for (const auto& name : names)
            {
                const auto& where = *portReport.unsupported.Find(name);
                warnings.push_back("UNSUPPORTED_GSC_API " + (StartsWith(name, "GSC ") ? name.substr(4) : name) + " (" + std::to_string(where.size()) + " uses, first " + where[0] +
                                   "): stubbed, reports at runtime");
            }
        }
        {
            std::set<std::string> known;
            for (const auto& a : animtrees)
                known.insert(Lower(a));
            for (const auto& [t, _] : coreTrees)
                known.insert(t);
            std::vector<std::string> trees(portReport.animtrees.order.begin(), portReport.animtrees.order.end());
            std::sort(trees.begin(), trees.end());
            for (const auto& tree : trees)
            {
                if (known.count(Lower(tree)))
                    continue;
                const auto& users = *portReport.animtrees.Find(tree);
                std::set<std::string> unique(users.begin(), users.end());
                warnings.push_back("UNSUPPORTED_XANIM animtree " + tree + " used by " + mod::Join(std::vector<std::string>(unique.begin(), unique.end()), ", ") +
                                   ": WaW animations are not converted yet");
            }
        }
        warnings.push_back("scripts: " + std::to_string(portReport.ported.size()) + " WaW scripts ported, " + std::to_string(portReport.extracted.size()) +
                           " framework functions extracted, " + std::to_string(portReport.fx.size()) + " FX names referenced");
        if (!portReport.coreOverrides.empty())
            warnings.push_back("scripts: the map overrides WaW framework scripts BO2 replaces with its own (behaviour changes there are not ported): " +
                               mod::Join(portReport.coreOverrides, ", "));
        // models the scripts precache / set / attach, in the emitted code
        static const pyre::Regex modelCall("\\b(?:waw_)?(?:precachemodel|setmodel|setviewmodel|attach)\\s*\\(\\s*\"([^\"]+)\"", pyre::IGNORECASE);
        static const pyre::Regex precacheCall("\\b(?:waw_)?precachemodel\\s*\\(\\s*\"([^\"]+)\"", pyre::IGNORECASE);
        static const pyre::Regex shaderCall("\\b(?:precacheshader|setshader)\\s*\\(\\s*\"([^\"]+)\"", pyre::IGNORECASE);
        const auto coreText = files::ReadText(projectRoot / "maps/mp/waw/_waw2bo2_core.gsc");
        std::set<std::string> wanted;
        for (const auto& m : FindAllGroup(modelCall, coreText))
            wanted.insert(m);
        std::vector<std::string> portedTexts{coreText};
        for (const auto& path : portReport.ported)
        {
            const auto text = SourceText(sources, path + ".gsc");
            for (const auto& m : FindAllGroup(modelCall, text))
                wanted.insert(m);
            if (StartsWith(path, "xmodelalias\\"))
                for (const auto& t : gsc::Tokenize(text))
                    if (t.kind == gsc::Kind::String)
                        wanted.insert(t.text.substr(1, t.text.size() - 2));
            const auto* own = sources.text->Find(path + ".gsc");
            portedTexts.push_back(own ? *own : std::string());
        }
        auto wrapped = WrappedModelLiterals(portedTexts);
        {
            // stock_wrapped_models: wrapped literals of stock scripts the map does not override
            std::vector<std::string> texts;
            for (const auto& path : portReport.ported)
                texts.push_back(SourceText(sources, path + ".gsc"));
            Resolver local(modelRoots, nullptr);
            for (const auto& name : WrappedModelLiterals(texts))
                if (local.Find("xmodel", name))
                    wrapped.insert(name);
        }
        wanted.insert(wrapped.begin(), wrapped.end());
        std::set<std::string> stockModels;
        if (api.stockAssets.count("xmodel"))
            stockModels = api.stockAssets.at("xmodel");
        if (stockPerks)
        {
            // BO2's perk machine models come from mod.ff
            for (const auto& m : port::stockperks::MachineModels(bo2Root))
            {
                wanted.erase(m);
                stockModels.insert(Lower(m));
            }
        }
        // recover_script_models: runtime model dependencies through the complete WaW lookup
        std::set<std::string> models;
        {
            Resolver resolver(modelRoots, stock ? &*stock : nullptr);
            Json dependencies = Json::array();
            for (const auto& name : wanted)
            {
                const auto path = resolver.Find("xmodel", name);
                Json node{{"name", name}, {"status", path ? "resolved" : "missing_waw_source"}};
                if (path)
                {
                    models.insert(name);
                    node["source"] = Str(*path);
                    node["provenance"] = "compiled_waw";
                }
                dependencies.push_back(node);
            }
            modelRoots = Dedupe(resolver.Roots());
            scripts["requested_models"] = std::vector<std::string>(wanted.begin(), wanted.end());
            scripts["model_sources"] = dependencies;
        }
        std::set<std::string> missing;
        for (const auto& name : wanted)
            if (!models.count(name) && !stockModels.count(Lower(name)))
                missing.insert(name);
        Json guarded = Json::array();
        for (const auto& path : files::RGlob(projectRoot / "maps/mp/waw", ".gsc"))
        {
            const auto [source, names] = port::GuardMissingModelCalls(files::ReadText(path), missing);
            if (!names.empty())
            {
                files::WriteText(path, source);
                for (const auto& name : names)
                    guarded.push_back(Json{{"script", Generic(path.lexically_relative(projectRoot))}, {"model", name}});
            }
        }
        scripts["guarded_missing_model_calls"] = guarded;
        for (const auto& name : wanted)
            if (!models.count(name) && !stockModels.count(Lower(name)))
                warnings.push_back("UNSUPPORTED_ASSET xmodel " + name + ": used by the map scripts, absent from the supplied WaW zones and searched WaW sources");
        // the named models the scripts precache, in the first frame
        std::set<std::string> precached;
        for (const auto& path : portReport.ported)
        {
            const auto* own = sources.text->Find(path + ".gsc");
            for (const auto& m : FindAllGroup(precacheCall, own ? *own : std::string()))
                precached.insert(m);
        }
        for (const auto& m : FindAllGroup(precacheCall, coreText))
            precached.insert(m);
        precached.insert(models.begin(), models.end());
        precached.insert(wrapped.begin(), wrapped.end());
        std::vector<std::string> precachedList;
        for (const auto& m : precached)
            if (models.count(m) || stockModels.count(Lower(m)))
                precachedList.push_back(m);
        files::WriteText(projectRoot / "maps/mp/waw/_waw2bo2_precache.gsc", port::PrecacheSource(precachedList));
        scripts["precached_models"] = precachedList;
        // HUD materials the scripts precache or draw
        std::set<std::string> shaders;
        for (const auto& m : FindAllGroup(shaderCall, files::ReadText(port::CompatDir() / "_waw2bo2_compat.gsc")))
            shaders.insert(m);
        std::set<std::string> sourceShaders;
        for (const auto& m : FindAllGroup(shaderCall, coreText))
            sourceShaders.insert(m);
        const auto builtinMaterials = [&](const std::string& text)
        {
            for (const auto& [b, m] : ENGINE_BUILTIN_MATERIALS)
                if (pyre::Regex("\\b" + b + "\\s*\\(", pyre::IGNORECASE).Search(text))
                    shaders.insert(m);
        };
        for (const auto& path : portReport.ported)
        {
            const auto* own = sources.text->Find(path + ".gsc");
            auto text = own && !own->empty() ? *own : (sources.stock->Find(path + ".gsc") ? *sources.stock->Find(path + ".gsc") : std::string());
            for (const auto& m : FindAllGroup(shaderCall, text))
                sourceShaders.insert(m);
            builtinMaterials(text);
        }
        builtinMaterials(coreText);
        shaders.insert(sourceShaders.begin(), sourceShaders.end());
        std::set<std::string> stockMaterials;
        if (api.stockAssets.count("material"))
            stockMaterials = api.stockAssets.at("material");
        const std::set<std::string> wawHud(portReport.wawHudMaterials.begin(), portReport.wawHudMaterials.end());
        Json hud = Json::array();
        for (const auto& name : shaders)
        {
            bool wawHas = false;
            for (const auto& r : modelRoots)
                wawHas |= fs::exists(r / assets::MaterialPath(name), e);
            if (stockMaterials.count(Lower(name)) && !wawHud.count(name) && !(sourceShaders.count(name) && wawHas))
                continue;
            if (wawHas)
                hud.push_back(name);
            else
                warnings.push_back("UNSUPPORTED_ASSET material " + name + ": drawn by the map scripts, not in any WaW zone dump nor a stock BO2 zone");
        }
        scripts["hud_materials"] = hud;
        return finish(models);
    }
} // namespace w2b::pipeline
