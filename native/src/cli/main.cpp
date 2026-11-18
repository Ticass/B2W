// waw2bo2n: the converter's native steps, one subcommand each.
#include "core/files.h"
#include "core/share_files.h"
#include "assets/resolver.h"
#include "audio/stage.h"
#include "audio/flac_batch.h"
#include "shaders/stage.h"
#include "sound/stage.h"
#include "textures/iwd_service.h"
#include "textures/stage.h"
#include "core/parallel.h"
#include "core/pytext.h"
#include "effects/stage.h"
#include "gsc/parse.h"
#include "core/pyre.h"
#include "gsc/port/api.h"
#include "gsc/port/portmap.h"
#include "gsc/port/rewrites.h"
#include "gsc/port/staging.h"
#include "gsc/port/zone_scripts.h"
#include "materials/donor_index.h"
#include "materials/prepare.h"
#include "weapons/stage.h"
#include "weapons/water_bullets.h"
#include "physics/physics.h"
#include "mapents/entities.h"
#include "paths/paths.h"
#include "barriers/barriers.h"
#include "models/models.h"
#include "models/identities.h"
#include "models/material_identity.h"
#include "models/surface_fixes.h"
#include "world/collision_stage.h"
#include "world/fbx_stage.h"
#include "world/grid_stage.h"
#include "world/inspect.h"
#include "world/primary_lights.h"
#include "world/zone_manifest.h"
#include "world/probe_stage.h"
#include "world/vertices.h"

#include <functional>
#include <map>
#include "gsc/tokens.h"

#include <set>

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <charconv>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "launcher/build.h"
#include "extract/stock.h"
#include "strings/localization.h"
#include "visions/visions.h"
#include "models/projectile_collision.h"
#include "pipeline/materials_stage.h"
#include "pipeline/modzone.h"
#include "pipeline/audit.h"
#include "pipeline/driver.h"
#include "pipeline/finish.h"
#include "pipeline/menu_stage.h"
#include "pipeline/menu_runtime.h"
#include "pipeline/rumbles.h"
#include <optional>
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
                   "       waw2bo2n world-vertices <world> <byte-offset> <count> <legacy|compensated>\n"
                   "       waw2bo2n world-fbx <request.json>\n"
                   "       waw2bo2n world-fbx <render-world> <output.fbx> [--surface-limit <count>]\n"
                   "       waw2bo2n collision-fbx <clip-world> <output.fbx> [--triangle-limit <count>]\n"
                   "       waw2bo2n collision-mesh <clip-world> <output-folder> [--records]\n"
                   "       waw2bo2n world-inspect <render-world> <clip-world>\n"
                   "       waw2bo2n static-models <render-world> <model-folder> <report.json>\n"
                   "       waw2bo2n dds2iwi <source.dds|folder> <output.iwi|folder>\n"
                   "       waw2bo2n audio-stage --project <folder> --output <folder> [--decoder-text <s>] [--xwma <exe>] [--resampler <exe>]\n",
                   stderr);
        return 2;
    }

    int WorldVertices(const std::vector<std::string>& args)
    {
        if (args[3] != "legacy" && args[3] != "compensated")
            return Usage();
        std::ifstream input(w2b::files::FromUtf8(args[0]), std::ios::binary);
        if (!input.seekg(std::stoll(args[1])))
            throw std::runtime_error("could not seek to world vertex records");
        if (_setmode(_fileno(stdout), _O_BINARY) == -1)
            throw std::runtime_error("could not set binary output mode");
        w2b::world::DecodeVertices(input, std::cout, std::stoul(args[2]), args[3] == "compensated");
        if (!std::cout.flush())
            throw std::runtime_error("could not flush decoded world vertices");
        return 0;
    }

    int WorldFbx(const std::vector<std::string>& args)
    {
        if (args.size() == 1)
        {
            const auto report = w2b::world::StageWorldFbx(w2b::files::FromUtf8(args[0]));
            std::puts(report.dump().c_str());
            return 0;
        }
        if (args.size() != 2 && (args.size() != 4 || args[2] != "--surface-limit"))
            return Usage();
        std::optional<std::size_t> limit;
        if (args.size() == 4)
        {
            std::string_view text = args[3];
            if (text.starts_with('+'))
                text.remove_prefix(1);
            std::size_t count = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
            if (error != std::errc{} || end != text.data() + text.size() || !count)
                throw std::runtime_error("--surface-limit must be positive");
            limit = count;
        }
        const auto report = w2b::world::ExportWorldFbx(w2b::files::FromUtf8(args[0]),
                                                       w2b::files::FromUtf8(args[1]), limit);
        std::printf("wrote %s (%zu surfaces, %zu vertices)\n", args[1].c_str(),
                    report.at("surfaces").get<std::size_t>(), report.at("vertices").get<std::size_t>());
        return 0;
    }

    int DdsToIwi(const std::string& sourcePath, const std::string& outputPath)
    {
        namespace files = w2b::files;
        const auto source = files::FromUtf8(sourcePath);
        const auto output = files::FromUtf8(outputPath);
        const bool folder = files::fs::is_directory(source);
        std::vector<files::fs::path> sources;
        if (folder)
        {
            const auto lowercase = [](std::wstring text)
            {
                for (auto& c : text)
                    c = static_cast<wchar_t>(std::towlower(c));
                return text;
            };
            for (const auto& entry : files::fs::directory_iterator(source))
                if (lowercase(entry.path().extension().wstring()) == L".dds")
                    sources.push_back(entry.path());
            std::ranges::stable_sort(sources, {}, [&](const auto& path) { return lowercase(path.filename().wstring()); });
        }
        else
            sources.push_back(source);
        int result = 0;
        for (const auto& path : sources)
        {
            auto destination = folder ? output / path.filename() : output;
            if (folder)
                destination.replace_extension(".iwi");
            const auto name = files::Utf8(path.filename());
            try
            {
                const auto header = w2b::textures::ConvertFile(path, destination);
                std::printf("%s -> %s fmt=0x%x flags=0x%x %ux%u\n", name.c_str(),
                            files::Utf8(destination.filename()).c_str(), static_cast<unsigned>(header.format),
                            header.flags, header.width, header.height);
            }
            catch (const w2b::textures::TextureError& error)
            {
                std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
                result = 1;
            }
        }
        return result;
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
        if (args.contains("fx_table"))
            options.fxTable = args["fx_table"].get<std::map<std::string, std::string>>();
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
    using Arguments = std::vector<std::string>;

    // argparse-style: positionals in order, "--name value" options
    struct Parsed
    {
        std::vector<std::string> positional;
        std::map<std::string, std::string> options;
        const std::string* Option(const std::string& name) const
        {
            const auto it = options.find(name);
            return it == options.end() ? nullptr : &it->second;
        }
    };

    std::optional<Parsed> Parse(const Arguments& args, const std::set<std::string>& valued)
    {
        Parsed parsed;
        for (size_t i = 0; i < args.size(); i++)
        {
            if (args[i].starts_with("--"))
            {
                if (!valued.count(args[i]) || i + 1 >= args.size())
                    return std::nullopt;
                parsed.options[args[i]] = args[i + 1];
                i++;
            }
            else
                parsed.positional.push_back(args[i]);
        }
        return parsed;
    }

    // a build step: Python's "waw2bo2: error: <Kind>: <message>" on failure
    int Step(const std::function<int()>& run)
    {
        try
        {
            return run();
        }
        catch (const w2b::pipeline::StepError& error)
        {
            std::fflush(stdout);
            std::fprintf(stderr, "waw2bo2: error: %s: %s\n", error.kind.c_str(), error.what());
            return 1;
        }
    }

    int PackageCommand(const Arguments& args)
    {
        const auto p = Parse(args, {"--bo2", "--work", "--dest"});
        if (!p || p->positional.size() != 2 || !p->Option("--bo2") || !p->Option("--work"))
            return Usage();
        return Step([&] {
            namespace fs = std::filesystem;
            const auto& project = p->positional[1];
            std::error_code ec;
            const auto stage = fs::weakly_canonical(fs::absolute(w2b::files::FromUtf8(p->positional[0])), ec);
            const auto work = fs::weakly_canonical(fs::absolute(w2b::files::FromUtf8(*p->Option("--work"))), ec);
            std::optional<fs::path> dest;
            if (const auto* d = p->Option("--dest"))
                dest = w2b::files::FromUtf8(*d);
            auto out = w2b::pipeline::Package(project, stage / "zone_out" / w2b::files::FromUtf8(project), work / "out", dest ? &*dest : nullptr);
            std::printf("packaged %s\n", w2b::files::Utf8(out.make_preferred()).c_str());
            std::printf("launch: %s\n", w2b::pipeline::QuotedJoin(w2b::pipeline::LaunchCommand(project, w2b::files::FromUtf8(*p->Option("--bo2")))).c_str());
            return 0;
        });
    }

    int CompileScriptsCommand(const Arguments& args)
    {
        const auto p = Parse(args, {"--bo2", "--unlinker"});
        if (!p || p->positional.size() != 2 || !p->Option("--bo2") || !p->Option("--unlinker"))
            return Usage();
        return Step([&] {
            namespace fs = std::filesystem;
            std::error_code ec;
            const auto resolve = [&](const std::string& s) { return fs::weakly_canonical(fs::absolute(w2b::files::FromUtf8(s)), ec); };
            const auto [compiled, log] = w2b::pipeline::CompileScripts(resolve(p->positional[0]), p->positional[1], resolve(*p->Option("--bo2")), resolve(*p->Option("--unlinker")));
            for (const auto& name : compiled)
                std::printf("compiled %s\n", name.c_str());
            return 0;
        });
    }

    // audit-zone <ff> <stage> <project> --bo2 B --stock S --unlinker U: builddriver._audit_zone
    int AuditZoneCommand(const Arguments& args)
    {
        const auto p = Parse(args, {"--bo2", "--stock", "--unlinker"});
        if (!p || p->positional.size() != 3 || !p->Option("--bo2") || !p->Option("--stock") || !p->Option("--unlinker"))
            return Usage();
        return Step([&] {
            const auto f = [](const std::string& s) { return w2b::files::FromUtf8(s); };
            w2b::pipeline::AuditZone(f(p->positional[0]), f(p->positional[1]), p->positional[2], f(*p->Option("--bo2")), f(*p->Option("--stock")), f(*p->Option("--unlinker")));
            return 0;
        });
    }

    // audit-material-args <zone> [<extra techset folder> ...]: tools/audit_material_args.py
    int AuditMaterialArgsCommand(const Arguments& args)
    {
        if (args.empty())
            return Usage();
        std::vector<std::filesystem::path> extra;
        for (size_t i = 1; i < args.size(); i++)
            extra.push_back(w2b::files::FromUtf8(args[i]));
        return w2b::pipeline::PrintMaterialAudit(w2b::files::FromUtf8(args[0]), extra);
    }

    int ExtractStep(const std::function<int()>& run)
    {
        try
        {
            return run();
        }
        catch (const w2b::extract::Error& error)
        {
            std::fflush(stdout);
            std::fprintf(stderr, "waw2bo2: error: %s: %s\n", error.kind.c_str(), error.what());
            return 1;
        }
    }

    // all2raw --settings-json J [--waw-cache-root R] [--bo2-cache-root R] [--refresh]: cli._extract_all
    int All2RawCommand(const Arguments& input)
    {
        auto args = input;
        const bool refresh = std::erase(args, std::string("--refresh")) > 0;
        const auto p = Parse(args, {"--settings-json", "--waw-cache-root", "--bo2-cache-root"});
        if (!p || !p->positional.empty() || !p->Option("--settings-json"))
            return Usage();
        return ExtractStep([&] {
            namespace l = w2b::launcher;
            const auto settings = l::Settings::FromJson(nlohmann::ordered_json::parse(*p->Option("--settings-json")));
            _wputenv_s(L"WAW2BO2_VERBOSE", settings.verbose ? L"1" : L"0");
            auto caches = l::CachePaths::ForSettings(settings);
            if (const auto* r = p->Option("--waw-cache-root"))
                caches.waw = w2b::files::FromUtf8(*r);
            if (const auto* r = p->Option("--bo2-cache-root"))
                caches.bo2 = w2b::files::FromUtf8(*r);
            const auto f = [](const std::string& s) { return w2b::files::FromUtf8(s); };
            w2b::extract::Prepare(f(settings.waw), f(settings.t4) / "Unlinker.exe", caches.waw, "T4", refresh);
            w2b::extract::Prepare(f(settings.bo2), f(settings.t6) / "Unlinker.exe", caches.bo2, "T6", refresh);
            std::error_code ec;
            if (!settings.fastfile.empty() && std::filesystem::is_regular_file(f(settings.fastfile), ec))
                w2b::extract::SourceDumps({f(settings.fastfile), f(settings.waw), f(settings.t4) / "Unlinker.exe"}, caches.waw, refresh);
            return 0;
        });
    }

    // extract-prepare <game> <unlinker> <cache root> T4|T6 --result <file> [--refresh]: all2raw.prepare
    int ExtractPrepareCommand(const Arguments& input)
    {
        auto args = input;
        const bool refresh = std::erase(args, std::string("--refresh")) > 0;
        const auto p = Parse(args, {"--result"});
        if (!p || p->positional.size() != 4 || !p->Option("--result"))
            return Usage();
        return ExtractStep([&] {
            const auto f = [](const std::string& s) { return w2b::files::FromUtf8(s); };
            const auto raw = w2b::extract::Prepare(f(p->positional[0]), f(p->positional[1]), f(p->positional[2]), p->positional[3], refresh);
            w2b::files::WriteBytes(f(*p->Option("--result")), nlohmann::ordered_json(w2b::extract::Str(raw)).dump());
            return 0;
        });
    }

    // extract-sources <fastfile> <waw> <t4 unlinker> <waw cache root> --result <file> [--refresh]: all2raw.source_dumps
    int ExtractSourcesCommand(const Arguments& input)
    {
        auto args = input;
        const bool refresh = std::erase(args, std::string("--refresh")) > 0;
        const auto p = Parse(args, {"--result"});
        if (!p || p->positional.size() != 4 || !p->Option("--result"))
            return Usage();
        return ExtractStep([&] {
            const auto f = [](const std::string& s) { return w2b::files::FromUtf8(s); };
            const auto dumps = w2b::extract::SourceDumps({f(p->positional[0]), f(p->positional[1]), f(p->positional[2])}, f(p->positional[3]), refresh);
            nlohmann::ordered_json out = nlohmann::ordered_json::object();
            for (const auto& [role, folder] : dumps)
                out[role] = w2b::extract::Str(folder);
            w2b::files::WriteBytes(f(*p->Option("--result")), out.dump());
            return 0;
        });
    }

    // build-map --settings-json J [--redump] [--build-root R] [--waw-cache-root R] [--bo2-cache-root R]
    int BuildMapCommand(const Arguments& input)
    {
        auto args = input;
        const bool redump = std::erase(args, std::string("--redump")) > 0;
        const auto p = Parse(args, {"--settings-json", "--build-root", "--waw-cache-root", "--bo2-cache-root"});
        if (!p || !p->positional.empty() || !p->Option("--settings-json"))
            return Usage();
        try
        {
            namespace l = w2b::launcher;
            const auto settings = l::Settings::FromJson(nlohmann::ordered_json::parse(*p->Option("--settings-json")));
            w2b::pipeline::BuildOptions options;
            options.redump = redump;
            if (const auto* r = p->Option("--build-root"))
                options.root = w2b::files::FromUtf8(*r);
            auto caches = l::CachePaths::ForSettings(settings);
            if (const auto* r = p->Option("--waw-cache-root"))
                caches.waw = w2b::files::FromUtf8(*r);
            if (const auto* r = p->Option("--bo2-cache-root"))
                caches.bo2 = w2b::files::FromUtf8(*r);
            options.caches = caches;
            return w2b::pipeline::BuildMap(settings, options);
        }
        catch (const w2b::pipeline::StepError& error)
        {
            std::fflush(stdout);
            std::fprintf(stderr, "waw2bo2: error: %s: %s\n", error.kind.c_str(), error.what());
        }
        catch (const w2b::extract::Error& error)
        {
            std::fflush(stdout);
            std::fprintf(stderr, "waw2bo2: error: %s: %s\n", error.kind.c_str(), error.what());
        }
        catch (const std::exception& error)
        {
            std::fflush(stdout);
            std::fprintf(stderr, "waw2bo2: error: RuntimeError: %s\n", error.what());
        }
        return 1;
    }

    // build-mod STAGE PROJECT --bo2 B --work W --unlinker U [--linker L] [--techset-dump D] [--scripts-ready F]
    int BuildModCommand(const Arguments& args)
    {
        const auto p = Parse(args, {"--bo2", "--work", "--unlinker", "--linker", "--techset-dump", "--scripts-ready"});
        if (!p || p->positional.size() != 2 || !p->Option("--bo2") || !p->Option("--work") || !p->Option("--unlinker"))
            return Usage();
        return Step([&] {
            w2b::pipeline::BuildModOptions o;
            o.stage = w2b::files::FromUtf8(p->positional[0]);
            o.project = p->positional[1];
            o.bo2 = w2b::files::FromUtf8(*p->Option("--bo2"));
            o.work = w2b::files::FromUtf8(*p->Option("--work"));
            o.unlinker = w2b::files::FromUtf8(*p->Option("--unlinker"));
            if (const auto* v = p->Option("--linker"))
                o.linker = w2b::files::FromUtf8(*v);
            if (const auto* v = p->Option("--techset-dump"))
                o.techsetDump = w2b::files::FromUtf8(*v);
            if (const auto* v = p->Option("--scripts-ready"))
                o.scriptsReady = w2b::files::FromUtf8(*v);
            w2b::pipeline::BuildMod(o);
            return 0;
        });
    }

    int BridgeLinkCommand(const Arguments& args)
    {
        const auto p = Parse(args, {"--linker", "--techset-dump", "--log"});
        if (!p || p->positional.size() != 2 || !p->Option("--linker"))
            return Usage();
        return Step([&] {
            std::optional<std::filesystem::path> techset, log;
            if (const auto* t = p->Option("--techset-dump"))
                techset = w2b::files::FromUtf8(*t);
            if (const auto* l = p->Option("--log"))
                log = w2b::files::FromUtf8(*l);
            return w2b::pipeline::BridgeLink(w2b::files::FromUtf8(p->positional[0]), p->positional[1], w2b::files::FromUtf8(*p->Option("--linker")),
                                             techset ? &*techset : nullptr, log ? &*log : nullptr);
        });
    }

    int PrintReport(const w2b::py::Json& report)
    {
        std::puts(report.dump(-1, ' ', true).c_str());
        return 0;
    }

    template <auto Stage>
    int JsonRequest(const Arguments& args)
    {
        return args.size() == 1 ? PrintReport(Stage(w2b::files::FromUtf8(args[0]))) : Usage();
    }

    w2b::py::Json ReadResolverMessage()
    {
        std::string line;
        if (!std::getline(std::cin, line))
            throw std::runtime_error("asset resolver input closed");
        return w2b::py::Json::parse(line);
    }

    int ResolveAssets(const Arguments& args)
    {
        if (!args.empty())
            return Usage();
        const auto request = ReadResolverMessage();
        const auto query = [](const w2b::py::Json& message) {
            PrintReport(message);
            std::fflush(stdout);
            return ReadResolverMessage();
        };
        w2b::assets::StockLookup lookup;
        w2b::assets::StockSounds sounds;
        if (request.at("hasStock").get<bool>())
        {
            lookup = [&](const w2b::assets::Asset& asset) -> std::optional<w2b::assets::StockRoot> {
                const auto reply = query({{"lookup", asset}});
                if (reply.is_null())
                    return std::nullopt;
                return w2b::assets::StockRoot{reply.at(0).get<std::string>(),
                                              w2b::files::FromUtf8(reply.at(1).get_ref<const std::string&>())};
            };
            sounds = [&]() { return query({{"soundAliases", true}}).get<std::set<std::string>>(); };
        }
        try
        {
            return PrintReport({{"result", w2b::assets::Expand(request, std::move(lookup), std::move(sounds))}});
        }
        catch (const w2b::weapons::WeaponError& error)
        {
            return PrintReport({{"weaponError", error.what()}});
        }
    }

    int Inspect(const Arguments& args)
    {
        if (args.size() != 2)
            return Usage();
        const auto info = w2b::world::InspectWorld(w2b::files::FromUtf8(args[0]), w2b::files::FromUtf8(args[1]));
        std::puts(w2b::py::JsonDumps(info, 2).c_str());
        return 0;
    }

    int StaticModels(const Arguments& args)
    {
        if (args.size() != 3)
            return Usage();
        const auto report = w2b::world::InspectStaticModels(w2b::files::FromUtf8(args[0]),
                                                           w2b::files::FromUtf8(args[1]));
        const auto formatted = w2b::py::JsonDumps(report, 2);
        std::ofstream output(w2b::files::FromUtf8(args[2]));
        output << formatted << '\n';
        output.close();
        if (!output)
            throw std::runtime_error("cannot write " + args[2]);
        const auto missing = report.at("missing_models").size();
        if (missing)
            throw std::runtime_error(std::to_string(missing) + " static models have no extracted GLTF; see " + args[2]);
        std::puts(formatted.c_str());
        return 0;
    }

    int CollisionFbx(const Arguments& args)
    {
        if (args.size() != 2 && (args.size() != 4 || args[2] != "--triangle-limit"))
            return Usage();
        std::optional<std::size_t> limit;
        if (args.size() == 4)
        {
            std::string_view value = args[3];
            if (value.starts_with('+'))
                value.remove_prefix(1);
            std::size_t count = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
            if (end != value.data() + value.size() || error == std::errc::invalid_argument ||
                (error == std::errc{} && count == 0))
                throw std::runtime_error("--triangle-limit must be positive");
            if (error == std::errc{})
                limit = count;
        }
        const auto count = w2b::world::ExportCollisionFbx(w2b::files::FromUtf8(args[0]),
                                                           w2b::files::FromUtf8(args[1]), limit);
        std::printf("wrote %s (%zu collision triangles)\n", args[1].c_str(), count);
        return 0;
    }

    struct Command
    {
        std::string_view name;
        int (*run)(const Arguments&);
    };

    constexpr Command commands[]{
        {"shaders-stage", ShadersStage}, {"audio-stage", AudioStage},
        {"dds2iwi", [](const Arguments& a) { return a.size() == 2 ? DdsToIwi(a[0], a[1]) : Usage(); }},
        {"dds-batch", JsonRequest<w2b::textures::ConvertBatch>},
        {"index-donors", JsonRequest<w2b::materials::IndexDonors>},
        {"convert-effects", JsonRequest<w2b::effects::StageEffects>},
        {"stage-weapon-files", JsonRequest<w2b::weapons::StageWeapons>},
        {"stage-water-bullets", JsonRequest<w2b::weapons::StageWaterBulletsRequest>},
        {"stage-weapon-models", JsonRequest<w2b::weapons::StageModels>},
        {"stage-viewmodel-shadows", JsonRequest<w2b::weapons::StageShadowVariants>},
        {"resolve-assets", ResolveAssets},
        {"map-physics", JsonRequest<w2b::physics::Run>},
        {"convert-map-entities", JsonRequest<w2b::mapents::Run>},
        {"stage-ai-paths", JsonRequest<w2b::paths::Run>},
        {"stage-barricades", JsonRequest<w2b::barriers::Run>},
        {"stage-localization", JsonRequest<w2b::strings::StageLocalization>},
        {"stage-visions", JsonRequest<w2b::visions::StageVisions>},
        {"recover-projectile-collision", JsonRequest<w2b::models::RecoverProjectileCollision>},
        {"stage-materials", JsonRequest<w2b::pipeline::StageMaterials>},
        {"stage-map-models", JsonRequest<w2b::models::Stage>},
        {"fix-model-surfaces", JsonRequest<w2b::models::StageSurfaceFixesRequest>},
        {"isolate-model-names", JsonRequest<w2b::models::IsolateModelNamesRequest>},
        {"isolate-material-names", JsonRequest<w2b::models::IsolateMaterialNamesRequest>},
        {"stage-rumbles", JsonRequest<w2b::pipeline::StageRumblesRequest>},
        {"stage-script-files", JsonRequest<w2b::gsc::port::StageScriptFilesRequest>},
        {"list-map-scripts", JsonRequest<w2b::gsc::port::MapScriptsRequest>},
        {"map-zone-scripts", JsonRequest<w2b::gsc::port::ZoneScriptsRequest>},
        {"write-map-zone", JsonRequest<w2b::world::WriteMapZoneRequest>},
        {"stage-menu-settings", JsonRequest<w2b::pipeline::StageMenuSettingsRequest>},
        {"stage-menu-runtime", JsonRequest<w2b::pipeline::StageMenuRuntimeRequest>},
        {"encode-streamed-flac", JsonRequest<w2b::audio::EncodeFlacBatch>},
        {"share-cache-files", JsonRequest<w2b::files::ShareIdenticalFiles>},
        {"effect-overlays", JsonRequest<w2b::effects::StageOverlayEffects>},
        {"stage-model-overlay-runtime", JsonRequest<w2b::effects::StageModelOverlayRuntimeRequest>},
        {"stage-effect-models", JsonRequest<w2b::models::StageEffectAssetsRequest>},
        {"prepare-materials", [](const Arguments& a)
        {
            if (a.size() != 1)
                return Usage();
            w2b::materials::PrepareMaterials(w2b::files::FromUtf8(a[0]));
            return 0;
        }},
        {"iwd-recovery", [](const Arguments& a)
        {
            if (!a.empty())
                return Usage();
            w2b::textures::ServeIwdRecovery(std::cin, std::cout);
            return 0;
        }},
        {"lightmaps", JsonRequest<w2b::textures::StageLightmaps>},
        {"light-grid", JsonRequest<w2b::world::StageGrid>},
        {"primary-lights", [](const Arguments& a)
        {
            if (a.size() != 3 || (a[2] != "keep" && a[2] != "disable"))
                return Usage();
            return PrintReport(w2b::world::StagePrimaryLights(w2b::files::FromUtf8(a[0]),
                        w2b::files::FromUtf8(a[1]), a[2] == "keep"));
        }},
        {"reflection-probes", JsonRequest<w2b::world::StageProbes>},
        {"port-map", [](const Arguments& a) { return a.size() == 1 ? PortMapCommand(a[0]) : Usage(); }},
        {"world-vertices", [](const Arguments& a) { return a.size() == 4 ? WorldVertices(a) : Usage(); }},
        {"world-fbx", WorldFbx},
        {"collision-fbx", CollisionFbx},
        {"collision-mesh", [](const Arguments& a)
        {
            if (a.size() != 2 && (a.size() != 3 || a[2] != "--records"))
                return Usage();
            return PrintReport(w2b::world::StageCollisionMesh(w2b::files::FromUtf8(a[0]),
                        w2b::files::FromUtf8(a[1]), a.size() == 3));
        }},
        {"package", PackageCommand}, {"compile-scripts", CompileScriptsCommand}, {"bridge-link", BridgeLinkCommand},
        {"build-map", BuildMapCommand}, {"build-mod", BuildModCommand}, {"audit-zone", AuditZoneCommand}, {"all2raw", All2RawCommand}, {"extract-prepare", ExtractPrepareCommand},
        {"extract-sources", ExtractSourcesCommand}, {"audit-material-args", AuditMaterialArgsCommand},
        {"inspect", Inspect}, {"world-inspect", Inspect},
        {"static-models", StaticModels},
        {"sounds-stage", [](const Arguments& a) { return a.size() == 1 ? SoundsStage(a[0]) : Usage(); }},
        // launcher-build <settings.json>: the launcher's Build Map flow, console on stdout
        {"launcher-build", [](const Arguments& a)
        {
            if (a.size() != 1)
                return Usage();
            namespace l = w2b::launcher;
            const auto settings = l::Discover(l::Settings::Load(w2b::files::FromUtf8(a[0])));
            l::ProcessRunner runner([](const std::string& line) { std::printf("%s\n", line.c_str()); std::fflush(stdout); }, settings.verbose);
            const int code = l::PerformBuild(settings, runner);
            std::printf("launcher-build: exit %d\n", code);
            return code;
        }},
        // launcher-check <settings.json>: the launcher rules for those settings, as JSON
        {"launcher-check", [](const Arguments& a)
        {
            if (a.size() != 1)
                return Usage();
            namespace l = w2b::launcher;
            const auto settings = l::Discover(l::Settings::Load(w2b::files::FromUtf8(a[0])));
            nlohmann::ordered_json out;
            out["settings"] = settings.ToJson();
            out["project_name"] = l::ProjectName(settings.fastfile);
            out["map_zone"] = w2b::files::Utf8(l::MapZone(w2b::files::FromUtf8(settings.fastfile)));
            const auto caches = l::CachePaths::ForSettings(settings);
            out["cache"] = {w2b::files::Utf8(caches.waw), w2b::files::Utf8(caches.bo2)};
            try
            {
                const auto paths = l::BuildPaths::ForSettings(settings);
                out["paths"] = {w2b::files::Utf8(paths.root), w2b::files::Utf8(paths.stage), w2b::files::Utf8(paths.stock),
                                w2b::files::Utf8(paths.wawDumps), w2b::files::Utf8(paths.mod), w2b::files::Utf8(paths.output)};
                out["complete"] = paths.Complete(settings.project, &settings.bo2_stock_perks);
                out["build_command"] = l::BuildCommand(settings);
            }
            catch (const std::exception& error)
            {
                out["paths"] = error.what();
            }
            out["checks"] = nlohmann::ordered_json::array();
            for (const auto& c : l::Preflight(settings))
                out["checks"].push_back({c.name, c.ready, c.detail, c.required});
            out["installed"] = l::Installed(settings);
            out["stamp"] = l::InputStamp(settings);
            std::fwrite(w2b::py::JsonDumps(out, 1).data(), 1, w2b::py::JsonDumps(out, 1).size(), stdout);
            return 0;
        }},
        {"gsc-tokens", [](const Arguments& a) { return a.size() == 2 ? GscTokens(a[0], a[1]) : Usage(); }},
        {"regex-test", [](const Arguments& a) { return a.size() == 2 ? RegexTest(a[0], a[1]) : Usage(); }},
        {"gsc-rewrite", [](const Arguments& a)
        {
            if (a.size() != 4)
                return Usage();
            w2b::gsc::port::SetCompatDir(w2b::files::FromUtf8(a[3]));
            return GscRewrite(a[0], a[1], a[2]);
        }},
    };
} // namespace

int wmain(const int argc, wchar_t** argv)
{
    if (argc < 2)
        return Usage();
    const auto command = Utf8(argv[1]);
    const auto found = std::ranges::find(commands, command, &Command::name);
    if (found == std::end(commands))
        return Usage();
    Arguments args;
    for (int i = 2; i < argc; ++i)
        args.push_back(Utf8(argv[i]));
    try
    {
        return found->run(args);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "waw2bo2n %s: %s\n", command.c_str(), error.what());
        return 1;
    }
}
