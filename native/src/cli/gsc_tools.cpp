// Script diagnostics (see gsc_tools.h).
#include "cli/gsc_tools.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/text.h"
#include "extract/cache.h"
#include "gsc/ast.h"
#include "gsc/port/api.h"
#include "gsc/port/clientport.h"
#include "gsc/port/link_check.h"
#include "gsc/port/portmap.h"
#include "gsc/port/sources.h"
#include "gsc/port/t6api.h"
#include "gsc/port/vm_rules.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <set>
#include <stdexcept>

namespace w2b::cli
{
    namespace fs = std::filesystem;

    namespace
    {
        void Print(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }

        std::string ReadFile(const fs::path& path)
        {
            std::string data;
            if (!files::ReadBytes(path, data))
                throw std::runtime_error("cannot read " + files::Utf8(path));
            return data;
        }

        // a map of the corpus: its main script's name and its script roots, as WaW loads them (archives, patch, map, mod)
        struct CorpusMap
        {
            std::string folder, name;
            std::vector<fs::path> roots;
        };

        std::vector<CorpusMap> CorpusMaps(const fs::path& corpus)
        {
            std::vector<CorpusMap> maps;
            std::vector<fs::path> folders;
            for (const auto& entry : fs::directory_iterator(corpus))
                if (entry.is_directory())
                    folders.push_back(entry.path());
            std::sort(folders.begin(), folders.end());
            for (const auto& folder : folders)
            {
                std::vector<fs::path> archives, zones;
                for (const auto& entry : fs::directory_iterator(folder))
                {
                    const auto name = files::Utf8(entry.path().filename());
                    (name.starts_with("iwd_") ? archives : zones).push_back(entry.path());
                }
                std::sort(archives.rbegin(), archives.rend()); // the later archive first
                std::sort(zones.begin(), zones.end());
                for (const auto& zone : zones)
                {
                    const auto name = files::Utf8(zone.filename()).substr(3);
                    if (name == "mod" || name.ends_with("_patch") || name.ends_with("_load") || name.starts_with("localized_"))
                        continue;
                    bool main = false;
                    for (const auto& root : zones)
                        main = main || fs::is_regular_file(root / "maps" / files::FromUtf8(name + ".gsc"));
                    if (!main)
                        continue;
                    CorpusMap map{files::Utf8(folder.filename()), name, archives};
                    for (const auto& part : {"ff_" + name + "_patch", "ff_" + name, std::string("ff_mod")})
                        if (fs::is_directory(folder / files::FromUtf8(part)))
                            map.roots.push_back(folder / files::FromUtf8(part));
                    maps.push_back(std::move(map));
                }
            }
            return maps;
        }
    } // namespace

    int GscCheck(const Arguments& args)
    {
        auto options = args;
        const auto dialect = std::erase(options, std::string("--bo2")) ? gsc::Dialect::Bo2 : gsc::Dialect::Waw;
        if (options.size() != 1 && (options.size() != 3 || options[1] != "--dump"))
            return 2;
        const auto root = files::FromUtf8(options[0]);
        std::vector<fs::path> scripts;
        if (fs::is_directory(root))
        {
            scripts = files::GlobSuffix(root, ".gsc");
            const auto client = files::GlobSuffix(root, ".csc");
            scripts.insert(scripts.end(), client.begin(), client.end());
            std::sort(scripts.begin(), scripts.end());
        }
        else
            scripts.push_back(root);
        std::string dump;
        int rejected = 0;
        gsc::port::VmCounts counts;
        for (const auto& path : scripts)
        {
            const auto name = files::Utf8(fs::is_directory(root) ? path.lexically_relative(root) : path);
            dump += "== " + name + "\n";
            try
            {
                const auto tokens = gsc::Tokenize(encoding::UniversalNewlines(encoding::Utf8SigReplace(ReadFile(path))));
                dump += gsc::DumpAst(gsc::ParseAst(tokens, dialect), tokens);
                // the translator's VM rules, and that their result still parses
                auto ported = tokens;
                counts.Add(gsc::port::ApplyVmRules(ported, {"runtime"}));
                gsc::ParseAst(gsc::Tokenize(gsc::Emit(ported)), dialect);
            }
            catch (const gsc::GscSyntaxError& error)
            {
                rejected++;
                dump += std::string("!! ") + error.what() + "\n";
                Print(name + ": " + error.what());
            }
        }
        Print(std::format("{} scripts, {} rejected; {} of {} loops may never end, {} switches, {} vectors, {} logic values, {} array and {} string comparisons",
                          scripts.size(), rejected, counts.watched, counts.loops, counts.switches, counts.vectors, counts.logic, counts.arrays,
                          counts.comparisons));
        if (options.size() == 3)
            files::WriteBytes(files::FromUtf8(options[2]), dump);
        return rejected ? 1 : 0;
    }

    int GscLink(const Arguments& args)
    {
        if (args.empty())
            return 2;
        const auto compat = extract::ResourceRoot() / "src/waw2bo2/compat";
        const auto api = gsc::port::T6Api::Load(compat / "t6_script_api.json", fs::path());
        const auto problems = gsc::port::LinkCheck(files::FromUtf8(args[0]), api, {args.begin() + 1, args.end()});
        for (const auto& problem : problems)
            Print(problem);
        Print(std::format("{} problems", problems.size()));
        return problems.empty() ? 0 : 1;
    }

    int GscRules(const Arguments& args)
    {
        if (args.size() != 3)
            return 2;
        auto tokens = gsc::Tokenize(encoding::UniversalNewlines(encoding::Utf8SigReplace(ReadFile(files::FromUtf8(args[0])))));
        const auto counts = gsc::port::ApplyVmRules(tokens, {args[2]});
        files::WriteBytes(files::FromUtf8(args[1]), gsc::Emit(tokens));
        Print(std::format("{} changes", counts.Changes()));
        return 0;
    }

    int GscPort(const Arguments& args)
    {
        if (args.size() < 4)
            return 2;
        const auto stock = files::FromUtf8(args[1]), bo2 = files::FromUtf8(args[2]), out = files::FromUtf8(args[3]);
        const std::set<std::string> only(args.begin() + 4, args.end());
        const auto compat = extract::ResourceRoot() / "src/waw2bo2/compat";
        gsc::port::SetCompatDir(compat);
        const auto api = gsc::port::T6Api::Load(compat / "t6_script_api.json", fs::path());
        gsc::port::PortMapOptions options;
        for (const auto& entry : fs::directory_iterator(bo2 / "raw/animtrees"))
            if (entry.path().extension() == ".atr")
                options.animtrees.insert(files::Utf8(entry.path().stem()));
        std::set<std::string> builtins;
        for (const auto* table : {&api.builtins, &api.methods})
            for (const auto& [name, _] : *table)
                builtins.insert(name);
        int failed = 0, total = 0;
        std::map<std::string, int> kinds;
        for (const auto& map : CorpusMaps(files::FromUtf8(args[0])))
        {
            if (!only.empty() && !only.count(map.name) && !only.count(map.folder))
                continue;
            total++;
            const auto root = out / files::FromUtf8(map.folder + "_" + map.name);
            std::error_code error;
            fs::remove_all(root, error);
            std::vector<std::string> problems;
            size_t modules = 0;
            try
            {
                gsc::port::Sources sources(map.roots, {}, stock, {});
                sources.BindZombieFramework("maps/" + map.name);
                const auto client = gsc::port::clientport::PortMapClient(sources, map.name, root,
                                                                         gsc::port::clientport::StagedClientModules(map.name, "zm_corpus_waw"), builtins, {});
                modules = client.contains("modules") ? client["modules"].size() : 0;
                auto mapOptions = options;
                mapOptions.clientPorted = modules != 0;
                gsc::port::Sources fresh(map.roots, {}, stock, {});
                fresh.BindZombieFramework("maps/" + map.name);
                const auto report = gsc::port::PortMap(fresh, api, map.name, root, mapOptions);
                problems = report.errors;
                for (const auto& [kind, where] : report.unsupported.values)
                    if (kind.find("VM rules") != std::string::npos)
                        problems.insert(problems.end(), where.begin(), where.end());
                for (const auto* file : {"_waw2bo2_compat.gsc", "_waw2bo2_fixes.gsc"})
                    fs::copy_file(compat / file, root / "maps/mp/waw" / file, fs::copy_options::overwrite_existing);
                fs::create_directories(root / "clientscripts/mp/waw");
                fs::copy_file(compat / "_waw2bo2_environment.csc", root / "clientscripts/mp/waw/_waw2bo2_environment.csc", fs::copy_options::overwrite_existing);
                for (auto& problem : gsc::port::LinkCheck(root, api))
                    // scripts the later stages write are not here
                    if (problem.find("_waw2bo2_") == std::string::npos || problem.find("does not exist") == std::string::npos)
                        problems.push_back(std::move(problem));
            }
            catch (const std::exception& error2)
            {
                problems.push_back(std::string("FAILED: ") + error2.what());
            }
            failed += !problems.empty();
            Print(std::format("{} {}: {} problems, {} client modules", map.folder, map.name, problems.size(), modules));
            for (const auto& problem : problems)
                Print("    " + problem);
        }
        Print(std::format("{} maps, {} with problems", total, failed));
        return failed ? 1 : 0;
    }
} // namespace w2b::cli
