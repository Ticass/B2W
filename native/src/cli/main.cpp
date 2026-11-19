// waw2bo2n: the converter's command line: the build and its diagnostics.
#include "cli/dev_tools.h"
#include "core/asset_paths.h"
#include "core/files.h"
#include "core/pytext.h"
#include "extract/cache.h"
#include "extract/stock.h"
#include "launcher/build.h"
#include "materials/waw_material_json.h"
#include "oat/run_unlinker.h"
#include "oat/waw_assets.h"
#include "oat/zone_dumper.h"
#include "pipeline/audit.h"
#include "pipeline/bridge_stage.h"
#include "pipeline/driver.h"
#include "pipeline/finish.h"
#include "pipeline/modzone.h"
#include "textures/stage.h"
#include "world/collision_stage.h"
#include "world/fbx_stage.h"
#include "world/inspect.h"
#include "world/vertices.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
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
        std::fputs("usage: waw2bo2n build-map --settings-json <json> | --settings-file <file> [--redump] [--build-root <folder>]\n"
                   "       waw2bo2n all2raw --settings-json <json> | --settings-file <file>\n"
                   "       waw2bo2n package <stage> <project> --bo2 <folder> --work <folder> [--dest <folder>]\n"
                   "       waw2bo2n launcher-build <settings.json>\n"
                   "       waw2bo2n stage-bridge | build-mod | compile-scripts | bridge-link <arguments>\n"
                   "diagnostics:\n"
                   "       waw2bo2n oat-unlink <Unlinker arguments>\n"
                   "       waw2bo2n oat-dump-assets <zone.ff> <output> <search;paths> <kind:name>...\n"
                   "       waw2bo2n oat-check-materials <zone.ff> <dump root> [search paths...]\n"
                   "       waw2bo2n audit-zone | audit-material-args <arguments>\n"
                   "       waw2bo2n inspect | world-inspect <render-world> <clip-world>\n"
                   "       waw2bo2n world-vertices <world> <byte-offset> <count> <legacy|compensated>\n"
                   "       waw2bo2n world-fbx <render-world> <output.fbx> [--surface-limit <count>]\n"
                   "       waw2bo2n collision-fbx <clip-world> <output.fbx> [--triangle-limit <count>]\n"
                   "       waw2bo2n collision-mesh <clip-world> <output-folder> [--records]\n"
                   "       waw2bo2n static-models <render-world> <model-folder> <report.json>\n"
                   "       waw2bo2n dds2iwi <source.dds|folder> <output.iwi|folder>\n"
                   "       waw2bo2n knowledge-report [--ledger <folder>]... [--bundle <zip>]... [--report <json>]... [--record] [--write <json>] [--top <n>]\n"
                   "       waw2bo2n ipak-check <file.ipak>\n"
                   "       waw2bo2n crash-summary <dump.dmp>...\n",
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

    // the launcher settings of --settings-json J, or of --settings-file F (the
    // same JSON in a file, for shells that mangle quoted arguments); nullopt
    // unless exactly one is given
    std::optional<std::string> SettingsText(const Parsed& p)
    {
        const auto* json = p.Option("--settings-json");
        const auto* file = p.Option("--settings-file");
        if (!json == !file)
            return std::nullopt;
        return file ? w2b::files::ReadText(w2b::files::FromUtf8(*file)) : *json;
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

    // all2raw --settings-json J | --settings-file F [--waw-cache-root R] [--bo2-cache-root R] [--refresh]: cli._extract_all
    int All2RawCommand(const Arguments& input)
    {
        auto args = input;
        const bool refresh = std::erase(args, std::string("--refresh")) > 0;
        const auto p = Parse(args, {"--settings-json", "--settings-file", "--waw-cache-root", "--bo2-cache-root"});
        const auto text = p ? SettingsText(*p) : std::nullopt;
        if (!text || !p->positional.empty())
            return Usage();
        return ExtractStep([&] {
            namespace l = w2b::launcher;
            // blank fields as the launcher fills them (the bundled tools, detected games)
            const auto settings = l::Discover(l::Settings::FromJson(nlohmann::ordered_json::parse(*text)));
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

    // build-map --settings-json J | --settings-file F [--redump] [--build-root R] [--waw-cache-root R] [--bo2-cache-root R]
    // stage-bridge <stage> <project> --gfx ... (cli.py stage-bridge)
    // oat-unlink <Unlinker.exe arguments>: OpenAssetTools' Unlinker, linked into this program
    int OatUnlinkCommand(const Arguments& args)
    {
        const auto run = w2b::oat::RunUnlinker(args);
        std::fwrite(run.log.data(), 1, run.log.size(), stdout);
        return run.exitCode;
    }

    // oat-dump-assets <zone.ff> <output> <search;paths> <kind:name>...: named assets of one zone
    int OatDumpAssetsCommand(const Arguments& args)
    {
        if (args.size() < 4)
            return Usage();
        std::vector<std::filesystem::path> search;
        for (const auto part : w2b::py::Split(args[2], ";"))
            if (!part.empty())
                search.push_back(w2b::files::FromUtf8(std::string(part)));
        std::vector<std::pair<std::string, std::string>> assets;
        for (size_t i = 3; i < args.size(); i++)
        {
            const auto colon = args[i].find(':');
            if (colon == std::string::npos)
                return Usage();
            assets.emplace_back(args[i].substr(0, colon), args[i].substr(colon + 1));
        }
        const auto run = w2b::oat::DumpAssets(w2b::files::FromUtf8(args[0]), search, assets, w2b::files::FromUtf8(args[1]));
        std::fwrite(run.log.data(), 1, run.log.size(), stdout);
        return run.ok ? 0 : 1;
    }

    // oat-check-materials <zone.ff> <dump root> [search path]...: every material of the zone read
    // as an object must match the file the Unlinker dumped for it, both ways
    int OatCheckMaterialsCommand(const Arguments& args)
    {
        if (args.size() < 2)
            return Usage();
        w2b::oat::WawZone zone{w2b::files::FromUtf8(args[0]), {}};
        for (size_t i = 2; i < args.size(); i++)
            zone.searchPaths.push_back(w2b::files::FromUtf8(args[i]));
        const auto root = w2b::files::FromUtf8(args[1]);
        size_t checked = 0, missing = 0, different = 0;
        for (const auto& name : w2b::oat::MaterialNames(zone))
        {
            const auto file = root / w2b::assets::MaterialPath(name);
            if (!std::filesystem::is_regular_file(file))
            {
                missing++;
                continue;
            }
            const auto dumped = nlohmann::ordered_json::parse(w2b::files::ReadText(file));
            const auto object = w2b::oat::ReadMaterial(zone, name);
            checked++;
            if (!object)
            {
                if (different++ < 5)
                    std::printf("not read: %s\n", name.c_str());
                continue;
            }
            const auto written = w2b::materials::ToOatJson(*object);
            if (w2b::materials::FromOatJson(dumped) != *object || written != dumped)
                if (different++ < 5)
                    std::printf("differs: %s %s\n", name.c_str(), nlohmann::ordered_json::diff(dumped, written).dump().substr(0, 400).c_str());
        }
        std::printf("materials: %zu checked, %zu differ, %zu without a dump file\n", checked, different, missing);
        return different ? 1 : 0;
    }

    int StageBridgeCommand(const Arguments& args)
    {
        try
        {
            return w2b::pipeline::StageBridge(w2b::pipeline::BridgeStageOptions::Parse(args));
        }
        catch (const w2b::pipeline::StepError& error)
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

    int BuildMapCommand(const Arguments& input)
    {
        auto args = input;
        const bool redump = std::erase(args, std::string("--redump")) > 0;
        const auto p = Parse(args, {"--settings-json", "--settings-file", "--build-root", "--waw-cache-root", "--bo2-cache-root"});
        const auto text = p ? SettingsText(*p) : std::nullopt;
        if (!text || !p->positional.empty())
            return Usage();
        try
        {
            namespace l = w2b::launcher;
            // blank fields as the launcher fills them (the bundled tools, detected games)
            const auto settings = l::Discover(l::Settings::FromJson(nlohmann::ordered_json::parse(*text)));
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
        // the build, as the launcher runs it
        {"build-map", BuildMapCommand},
        {"all2raw", All2RawCommand},
        {"package", PackageCommand},
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
        // single build steps
        {"stage-bridge", StageBridgeCommand},
        {"build-mod", BuildModCommand},
        {"compile-scripts", CompileScriptsCommand},
        {"bridge-link", BridgeLinkCommand},
        // diagnostics
        {"oat-unlink", OatUnlinkCommand},
        {"oat-dump-assets", OatDumpAssetsCommand},
        {"oat-check-materials", OatCheckMaterialsCommand},
        {"audit-zone", AuditZoneCommand},
        {"audit-material-args", AuditMaterialArgsCommand},
        {"inspect", Inspect},
        {"world-inspect", Inspect},
        {"world-vertices", [](const Arguments& a) { return a.size() == 4 ? WorldVertices(a) : Usage(); }},
        {"world-fbx", WorldFbx},
        {"collision-fbx", CollisionFbx},
        {"collision-mesh", [](const Arguments& a)
        {
            if (a.size() != 2 && (a.size() != 3 || a[2] != "--records"))
                return Usage();
            return PrintReport(w2b::world::StageCollisionMesh(w2b::files::FromUtf8(a[0]), w2b::files::FromUtf8(a[1]), a.size() == 3));
        }},
        {"static-models", StaticModels},
        {"dds2iwi", [](const Arguments& a) { return a.size() == 2 ? DdsToIwi(a[0], a[1]) : Usage(); }},
        {"knowledge-report", w2b::cli::KnowledgeReport},
        {"ipak-check", w2b::cli::IpakCheck},
        {"crash-summary", w2b::cli::CrashSummary},
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
