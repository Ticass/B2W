// waw2bo2n: the converter's native steps, one subcommand each.
#include "core/files.h"
#include "audio/stage.h"
#include "shaders/stage.h"
#include "sound/stage.h"
#include "core/parallel.h"
#include "core/pytext.h"
#include "gsc/parse.h"
#include "core/pyre.h"
#include "gsc/port/api.h"
#include "gsc/port/portmap.h"
#include "gsc/port/rewrites.h"

#include <functional>
#include <map>
#include "gsc/tokens.h"

#include <set>

#include <windows.h>

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace
{
    std::string Utf8(const wchar_t* text)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
        return out;
    }

    int Usage()
    {
        std::fputs("usage: waw2bo2n shaders-stage --dest <folder> <root>...\n"
                   "       waw2bo2n audio-stage --project <folder> --output <folder> [--decoder-text <s>] [--xwma <exe>] [--resampler <exe>]\n",
                   stderr);
        return 2;
    }

    int ShadersStage(const std::vector<std::string>& args)
    {
        std::string dest;
        std::vector<std::string> roots;
        for (size_t i = 0; i < args.size(); i++)
        {
            if (args[i] == "--dest" && i + 1 < args.size())
                dest = args[++i];
            else
                roots.push_back(args[i]);
        }
        if (dest.empty())
            return Usage();
        const auto report = w2b::shaders::Stage(roots, w2b::files::FromUtf8(dest));
        std::printf("shaders: %lld translated, %lld unsupported\n", report["translated"].get<long long>(), report["unsupported"].get<long long>());
        return 0;
    }

    // gsc-tokens <list.txt> <out.jsonl>: tokenize each UTF-8 file of the list
    // (as gsc.tokenize sees its text) and write one JSON line per file
    int GscTokens(const std::string& listFile, const std::string& outFile)
    {
        std::string list;
        if (!w2b::files::ReadBytes(w2b::files::FromUtf8(listFile), list))
            return Usage();
        std::vector<std::string> paths;
        for (const auto line : w2b::py::SplitLines(list))
            if (!line.empty())
                paths.emplace_back(line);
        std::vector<std::string> lines(paths.size());
        w2b::parallel::For(paths.size(),
                           [&](const size_t i)
                           {
                               std::string text;
                               w2b::files::ReadBytes(w2b::files::FromUtf8(paths[i]), text);
                               nlohmann::ordered_json row;
                               row["path"] = paths[i];
                               try
                               {
                                   nlohmann::ordered_json tokens = nlohmann::ordered_json::array();
                                   for (const auto& t : w2b::gsc::Tokenize(text))
                                       tokens.push_back({w2b::gsc::KindName(t.kind), t.text, t.pre, t.line});
                                   row["tokens"] = std::move(tokens);
                                   const auto repaired = w2b::gsc::RepairBlockComments(text);
                                   row["repair"] = {repaired.first, repaired.second};
                                   const auto script = w2b::gsc::Parse(text, "x");
                                   row["includes"] = script.includes;
                                   row["animtree"] = script.animtree ? nlohmann::ordered_json(*script.animtree) : nlohmann::ordered_json();
                                   nlohmann::ordered_json functions = nlohmann::ordered_json::array();
                                   for (const auto& [low, f] : script.functions)
                                       functions.push_back({low, f.name, f.params, f.start, f.bodyOpen, f.bodyClose,
                                                            f.animtree ? nlohmann::ordered_json(*f.animtree) : nlohmann::ordered_json()});
                                   row["functions"] = std::move(functions);
                                   nlohmann::ordered_json refs = nlohmann::ordered_json::array();
                                   for (const auto& r : w2b::gsc::References(script.tokens))
                                       refs.push_back({r.index, r.name, r.qualifier ? nlohmann::ordered_json(*r.qualifier) : nlohmann::ordered_json(),
                                                       r.qualIndex ? nlohmann::ordered_json(*r.qualIndex) : nlohmann::ordered_json(), r.method, r.pointer});
                                   row["refs"] = std::move(refs);
                               }
                               catch (const w2b::gsc::GscSyntaxError& error)
                               {
                                   row["error"] = error.what();
                               }
                               lines[i] = row.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
                           });
        std::string out;
        for (const auto& line : lines)
            out += line + "\n";
        w2b::files::WriteBytes(w2b::files::FromUtf8(outFile), out);
        return 0;
    }

    // gsc-rewrite <name> <list.txt> <out.jsonl>: apply one gscport rewrite to
    // each file; one JSON line {"path", "count", "text"} or {"path", "error"}
    int GscRewrite(const std::string& name, const std::string& listFile, const std::string& outFile)
    {
        namespace gp = w2b::gsc::port;
        using Fn = std::function<int(gp::Tokens&)>;
        const std::map<std::string, Fn> rewrites{
            {"neutralize_animations", [](gp::Tokens& t) { gp::NeutralizeAnimations(t, "tree"); return 0; }},
            {"bridge_contact_kill_attacks", [](gp::Tokens& t) { return gp::BridgeContactKillAttacks(t, w2b::gsc::Parse(w2b::gsc::Emit(t), "x")); }},
            {"fix_syntax", gp::FixSyntax},
            {"split_call_receivers", gp::SplitCallReceivers},
            {"rename_level_fields", gp::RenameLevelFields},
            {"bridge_poi_record_comparisons", gp::BridgePoiRecordComparisons},
            {"bridge_revive_reads", gp::BridgeReviveReads},
            {"bridge_drinking_flag", gp::BridgeDrinkingFlag},
            {"bridge_score_writes", gp::BridgeScoreWrites},
            {"bridge_player_stat_reads", gp::BridgePlayerStatReads},
            {"bridge_dynamic_hud_materials", gp::BridgeDynamicHudMaterials},
            {"mirror_team_zombie_vars", gp::MirrorTeamZombieVars},
            {"bridge_font_scales", gp::BridgeFontScales},
            {"keep_bo2_deathanims", gp::KeepBo2Deathanims},
            {"bridge_intermission_camera", gp::BridgeIntermissionCamera},
            {"bridge_launch_triggers", gp::BridgeLaunchTriggers},
            {"lower_maxhealth_assignments", gp::LowerMaxhealthAssignments},
            {"lower_health_assignments", gp::LowerHealthAssignments},
            {"lower_hud_fields", gp::LowerHudFields},
            {"lower_zombie_weapon_keys", gp::LowerZombieWeaponKeys},
            {"lower_undefined_size_loops", gp::LowerUndefinedSizeLoops},
            {"wrap_integer_literals", gp::WrapIntegerLiterals},
            {"lower_stationary_fx_carriers", gp::LowerStationaryFxCarriers},
        };
        const auto fn = rewrites.find(name);
        if (fn == rewrites.end())
            return Usage();
        std::string list;
        w2b::files::ReadBytes(w2b::files::FromUtf8(listFile), list);
        std::vector<std::string> paths;
        for (const auto line : w2b::py::SplitLines(list))
            if (!line.empty())
                paths.emplace_back(line);
        std::vector<std::string> lines(paths.size());
        w2b::parallel::For(paths.size(),
                           [&](const size_t i)
                           {
                               std::string text;
                               w2b::files::ReadBytes(w2b::files::FromUtf8(paths[i]), text);
                               nlohmann::ordered_json row;
                               row["path"] = paths[i];
                               try
                               {
                                   auto tokens = w2b::gsc::Tokenize(text);
                                   row["count"] = fn->second(tokens);
                                   row["text"] = w2b::gsc::Emit(tokens);
                               }
                               catch (const std::exception& error)
                               {
                                   row["error"] = error.what();
                               }
                               lines[i] = row.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
                           });
        std::string out;
        for (const auto& line : lines)
            out += line + "\n";
        w2b::files::WriteBytes(w2b::files::FromUtf8(outFile), out);
        return 0;
    }

    // regex-test <cases.jsonl> <out.jsonl>: {"pattern","flags","text","repl"}
    // -> finditer spans/groups, sub, split, match, fullmatch (or the error)
    int RegexTest(const std::string& casesFile, const std::string& outFile)
    {
        std::string text;
        w2b::files::ReadBytes(w2b::files::FromUtf8(casesFile), text);
        std::vector<std::string> cases;
        for (const auto line : w2b::py::SplitLines(text))
            if (!line.empty())
                cases.emplace_back(line);
        std::vector<std::string> lines(cases.size());
        w2b::parallel::For(cases.size(),
                           [&](const size_t i)
                           {
                               const auto c = nlohmann::ordered_json::parse(cases[i]);
                               nlohmann::ordered_json row;
                               try
                               {
                                   const w2b::pyre::Regex re(c["pattern"].get<std::string>(), c["flags"].get<unsigned>());
                                   const auto subject = c["text"].get<std::string>();
                                   nlohmann::ordered_json found = nlohmann::ordered_json::array();
                                   for (const auto& m : re.FindIter(subject))
                                   {
                                       nlohmann::ordered_json spans = nlohmann::ordered_json::array();
                                       for (size_t g = 0; g <= m.Groups(); g++)
                                           spans.push_back({m.Start(g), m.End(g)});
                                       found.push_back(spans);
                                   }
                                   row["finditer"] = found;
                                   row["sub"] = re.Sub(c["repl"].get<std::string>(), subject);
                                   row["split"] = re.Split(subject);
                                   const auto m = re.MatchAt(subject);
                                   row["match"] = m ? nlohmann::ordered_json({m->Start(), m->End()}) : nlohmann::ordered_json();
                                   const auto f = re.FullMatch(subject);
                                   row["fullmatch"] = f ? nlohmann::ordered_json({f->Start(), f->End()}) : nlohmann::ordered_json();
                               }
                               catch (const std::exception& error)
                               {
                                   row = nlohmann::ordered_json::object();
                                   row["error"] = error.what();
                               }
                               lines[i] = row.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
                           });
        std::string out;
        for (const auto& line : lines)
            out += line + "\n";
        w2b::files::WriteBytes(w2b::files::FromUtf8(outFile), out);
        return 0;
    }

    // sounds-stage <args.json>: {"project", "roots", "iwd_dirs", "names" (list|null), "globals" (str|null)}
    int SoundsStage(const std::string& argsFile)
    {
        std::string text;
        if (!w2b::files::ReadBytes(w2b::files::FromUtf8(argsFile), text))
            return Usage();
        const auto args = nlohmann::ordered_json::parse(text);
        w2b::sound::StageInput input;
        input.project = w2b::files::FromUtf8(args["project"].get<std::string>());
        input.roots = args["roots"].get<std::vector<std::string>>();
        input.iwdDirs = args["iwd_dirs"].get<std::vector<std::string>>();
        if (!args["names"].is_null())
            input.names = args["names"].get<std::set<std::string>>();
        if (!args["globals"].is_null())
            input.globalsFile = args["globals"].get<std::string>();
        const auto report = w2b::sound::Stage(input);
        std::printf("sounds: %zu aliases, %zu audio files, %zu errors\n", report["aliases"].size(), report["audio"].size(), report["errors"].size());
        return 0;
    }

    // port-map <args.json>: gscport.port_map. {"roots", "iwds", "stock", "variant_roots", "snapshot",
    // "api_cache", "compat_dir", "map_name", "out_root", "animtrees", "core_animtrees", "bo2_stock_perks",
    // "actor_types", "actor_template" (kind -> source | null), "stock_map", "client_ported", "bind" (bool),
    // "report" (output JSON path)}
    int PortMapCommand(const std::string& argsFile)
    {
        namespace port = w2b::gsc::port;
        const auto args = nlohmann::ordered_json::parse(w2b::files::ReadText(w2b::files::FromUtf8(argsFile)));
        const auto paths = [&](const char* key)
        {
            std::vector<std::filesystem::path> out;
            if (args.contains(key) && !args[key].is_null())
                for (const auto& p : args[key])
                    out.push_back(w2b::files::FromUtf8(p.get<std::string>()));
            return out;
        };
        port::SetCompatDir(w2b::files::FromUtf8(args["compat_dir"].get<std::string>()));
        std::optional<std::filesystem::path> stock;
        if (!args["stock"].is_null())
            stock = w2b::files::FromUtf8(args["stock"].get<std::string>());
        port::Sources sources(paths("roots"), paths("iwds"), stock, paths("variant_roots"));
        const auto api = port::T6Api::Load(w2b::files::FromUtf8(args["snapshot"].get<std::string>()), w2b::files::FromUtf8(args["api_cache"].get<std::string>()));
        const auto mapName = args["map_name"].get<std::string>();
        if (args.value("bind", false))
            sources.BindZombieFramework("maps/" + mapName);
        port::PortMapOptions options;
        options.animtrees = args["animtrees"].get<std::set<std::string>>();
        for (const auto& [tree, names] : args["core_animtrees"].items())
            options.coreAnimtrees[tree] = names.get<std::set<std::string>>();
        options.bo2StockPerks = args["bo2_stock_perks"].get<bool>();
        options.actorTypes = args["actor_types"].get<std::set<std::string>>();
        if (!args["actor_template"].is_null())
            options.actorTemplate = args["actor_template"].get<std::map<std::string, std::string>>();
        options.stockMap = args["stock_map"].get<bool>();
        options.clientPorted = args["client_ported"].get<bool>();
        const auto report = port::PortMap(sources, api, mapName, w2b::files::FromUtf8(args["out_root"].get<std::string>()), options);
        w2b::files::WriteBytes(w2b::files::FromUtf8(args["report"].get<std::string>()), report.ToJson().dump());
        std::printf("port-map: %zu ported, %zu errors\n", report.ported.size(), report.errors.size());
        return 0;
    }

    // audio-stage --project <content_source> --output <pcm> [--decoder-text <str>] [--xwma <exe>] [--resampler <exe>]
    int AudioStage(const std::vector<std::string>& args)
    {
        std::string project, output, decoder;
        w2b::audio::Tools tools;
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            if (args[i] == "--project")
                project = args[i + 1];
            else if (args[i] == "--output")
                output = args[i + 1];
            else if (args[i] == "--decoder-text")
                decoder = args[i + 1];
            else if (args[i] == "--xwma")
                tools.xwmaHelper = w2b::files::FromUtf8(args[i + 1]);
            else if (args[i] == "--resampler")
                tools.resampler = w2b::files::FromUtf8(args[i + 1]);
            else
                return Usage();
        }
        if (project.empty() || output.empty())
            return Usage();
        const auto report = w2b::audio::Stage(w2b::files::FromUtf8(project), w2b::files::FromUtf8(output), decoder, tools);
        std::printf("audio: %zu decoded, %zu errors\n", report["audio"].size(), report["errors"].size());
        return 0;
    }
} // namespace

int wmain(const int argc, wchar_t** argv)
{
    if (argc < 2)
        return Usage();
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++)
        args.push_back(Utf8(argv[i]));
    const std::string command = Utf8(argv[1]);
    try
    {
        if (command == "shaders-stage")
            return ShadersStage(args);
        if (command == "audio-stage")
            return AudioStage(args);
        if (command == "port-map" && args.size() == 1)
            return PortMapCommand(args[0]);
        if (command == "sounds-stage" && args.size() == 1)
            return SoundsStage(args[0]);
        if (command == "gsc-tokens" && args.size() == 2)
            return GscTokens(args[0], args[1]);
        if (command == "regex-test" && args.size() == 2)
            return RegexTest(args[0], args[1]);
        if (command == "gsc-rewrite" && args.size() == 4)
        {
            w2b::gsc::port::SetCompatDir(w2b::files::FromUtf8(args[3]));
            return GscRewrite(args[0], args[1], args[2]);
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "waw2bo2n %s: %s\n", command.c_str(), error.what());
        return 1;
    }
    return Usage();
}
