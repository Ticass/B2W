// The build pipeline (see driver.h). Order, messages and receipts follow
// builddriver.py; "== N." banners drive the launcher's step tracking.
#include "driver.h"

#include "audit.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"
#include "core/timeline.h"
#include "extract/stock.h"
#include "finish.h"
#include "menu_stage.h"
#include "modzone.h"
#include "knowledge.h"
#include "launcher/diagnostics.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace w2b::pipeline
{
    namespace l = launcher;

    namespace
    {
        std::mutex stepMutex;
        std::string currentStep;

        void Say(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }

        std::string S(fs::path p)
        {
            return files::Utf8(p.make_preferred());
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        void FinishProcess(void* handle, void* context)
        {
            DWORD code = 0;
            GetExitCodeProcess(static_cast<HANDLE>(handle), &code);
            static_cast<timeline::ProcessRecord*>(context)->Finish(handle, static_cast<int>(code));
        }

        // a step of the Python conversion worker, on this console (builddriver.run)
        void RunCli(const std::vector<std::string>& args)
        {
            {
                std::lock_guard lock(stepMutex);
                currentStep = args.front();
            }
            const auto command = l::CliCommand(args);
            timeline::ProcessRecord record(command);
            std::vector<std::wstring> env;
            if (const auto parent = record.ParentVariable(); !parent.empty())
                env.push_back(parent);
            if (!_wgetenv(L"PYTHONPATH"))
                env.push_back(L"PYTHONPATH=" + (l::ResourceRoot() / "src").wstring());
            std::fflush(stdout);
            const int code = process::RunInherited(command, env, FinishProcess, &record);
            if (code)
                throw StepError("RuntimeError", files::Utf8(files::FromUtf8(command.front()).filename()) + " failed (" + std::to_string(static_cast<unsigned>(code)) + ")");
        }

        void SetStep(const std::string& step)
        {
            std::lock_guard lock(stepMutex);
            currentStep = step;
        }

        std::string WorldName(const fs::path& dump, const std::string& stem)
        {
            const auto maps = dump / "waw2bo2/maps";
            if (IsFile(maps / files::FromUtf8(stem + ".d3dbsp.gfx.bin")))
                return stem;
            std::vector<std::string> worlds;
            std::error_code e;
            for (fs::directory_iterator it(maps, e), end; !e && it != end; it.increment(e))
            {
                const auto name = files::Utf8(it->path().filename());
                if (name.size() > 15 && name.ends_with(".d3dbsp.gfx.bin"))
                    worlds.push_back(name.substr(0, name.size() - 15));
            }
            std::sort(worlds.begin(), worlds.end());
            if (worlds.size() == 1)
            {
                Say("== map world \"" + worlds[0] + "\" found in " + stem + ".ff");
                return worlds[0];
            }
            if (!worlds.empty())
            {
                std::string list;
                for (size_t i = 0; i < worlds.size(); i++)
                    list += (i ? ", " : "") + worlds[i];
                throw StepError("RuntimeError", stem + ".ff holds several map worlds (" + list + "); choose that map's own fastfile");
            }
            return stem;
        }

        void EnsureLaa(const fs::path& linker)
        {
            std::string data;
            if (!files::ReadBytes(linker, data) || data.size() < 64 || data.compare(0, 2, "MZ") != 0)
                throw StepError("ValueError", "Not a PE executable: " + S(linker));
            uint32_t pe = 0;
            std::memcpy(&pe, data.data() + 0x3c, 4);
            if (pe + 24 > data.size() || data.compare(pe, 4, std::string("PE\0\0", 4)) != 0)
                throw StepError("ValueError", "Invalid PE header: " + S(linker));
            uint16_t flags = 0;
            std::memcpy(&flags, data.data() + pe + 22, 2);
            if (!(flags & 0x20))
            {
                flags |= 0x20;
                std::memcpy(data.data() + pe + 22, &flags, 2);
                files::WriteBytes(linker, data);
            }
        }

        void SortPaths(std::vector<fs::path>& paths)
        {
            std::stable_sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b)
                             {
                                 std::vector<std::string> ka, kb;
                                 for (const auto& p : a)
                                     ka.push_back(l::Lower(files::Utf8(p)));
                                 for (const auto& p : b)
                                     kb.push_back(l::Lower(files::Utf8(p)));
                                 return ka < kb;
                             });
        }

        // identity of the converter: a staged tree from another version is never resumed
        std::string ConverterDigest()
        {
            std::string data = l::Version();
            const auto package = l::ResourceRoot() / "src" / "waw2bo2";
            std::vector<fs::path> paths;
            std::error_code e;
            for (fs::recursive_directory_iterator it(package, e), end; !e && it != end; it.increment(e))
                paths.push_back(it->path());
            SortPaths(paths);
            for (const auto& p : paths)
            {
                const auto ext = files::Utf8(p.extension());
                if ((ext != ".py" && ext != ".json") || !IsFile(p) || files::Utf8(p).find("__pycache__") != std::string::npos)
                    continue;
                std::string bytes;
                files::ReadBytes(p, bytes);
                data += files::Utf8(p.lexically_relative(package).generic_wstring()) + std::string(1, '\0') + bytes;
            }
            return hash::Sha256Hex(data);
        }

        py::Json EditableFiles(const fs::path& projectRoot)
        {
            py::Json files = py::Json::object();
            std::error_code e;
            if (!fs::is_directory(projectRoot, e))
                return files;
            static const std::set<std::string> suffixes{".gsc", ".csc", ".csv", ".atr", ".json", ".str", ".txt", ".cfg", ".menu"};
            std::vector<fs::path> paths;
            for (fs::recursive_directory_iterator it(projectRoot, e), end; !e && it != end; it.increment(e))
                paths.push_back(it->path());
            SortPaths(paths);
            for (const auto& p : paths)
                if (suffixes.count(l::Lower(files::Utf8(p.extension()))) && files::Utf8(p.filename()) != "menu.json" && IsFile(p))
                    files[files::Utf8(p.lexically_relative(projectRoot).generic_wstring())] = extract::Stamp(p);
            return files;
        }

        void WriteTimelineSummary(const fs::path& source)
        {
            try
            {
                timeline::Merge(source);
                const auto summary = timeline::Summarize(timeline::Load(source));
                auto target = source;
                target.replace_extension(".json");
                files::WriteText(target, py::JsonDumps(summary, 1));
                char wall[32];
                std::snprintf(wall, sizeof wall, "%.1f", summary.value("wall", 0.0));
                Say(std::string("== build timeline: ") + wall + "s total; " + S(target));
            }
            catch (const std::exception& error)
            {
                Say(std::string("build timeline: not summarized (") + error.what() + ")");
            }
        }

        void Remember(const l::Settings& settings, const std::optional<fs::path>& root, const std::string& outcome, const std::string& error, const double since)
        {
            try
            {
                const auto stage = root ? extract::Resolve(*root) / "stage" : l::BuildPaths::ForSettings(settings).stage;
                const auto reportFile = stage / "zone_raw" / files::FromUtf8(settings.project) / knowledge::REPORT_NAME;
                std::optional<py::Json> report;
                if (IsFile(reportFile) && (settings.resume || extract::Stamp(reportFile)[1].get<long long>() / 1e9 >= since))
                    report = py::Json::parse(files::ReadText(reportFile));
                std::optional<std::string> step;
                {
                    std::lock_guard lock(stepMutex);
                    if (outcome == "failed" && !currentStep.empty())
                        step = currentStep;
                }
                const auto obs = knowledge::Observation(report ? &*report : nullptr, settings.fastfile, settings.project, l::Version(), outcome, step, error);
                const auto path = knowledge::Record(l::Path(settings.work) / "knowledge", obs);
                size_t gaps = 0;
                for (const auto& [_, symbols] : obs["gaps"].items())
                    gaps += symbols.size();
                Say("knowledge: " + outcome + ", " + std::to_string(gaps) + " untranslated features recorded in " + S(path));
            }
            catch (const std::exception& e)
            {
                Say(std::string("knowledge: not recorded (") + e.what() + ")");
            }
        }

        std::string Describe(const std::exception& error)
        {
            if (const auto* step = dynamic_cast<const StepError*>(&error))
                return step->kind + ": " + step->what();
            if (const auto* extraction = dynamic_cast<const extract::Error*>(&error))
                return extraction->kind + ": " + extraction->what();
            return std::string("RuntimeError: ") + error.what();
        }

        int DoBuild(const l::Settings& settings, const BuildOptions& options, const l::CachePaths& caches)
        {
            std::set<std::string> frameworks;
            std::error_code e;
            if (!settings.fastfile.empty())
                for (fs::directory_iterator it(l::Path(settings.fastfile).parent_path(), e), end; !e && it != end; it.increment(e))
                {
                    const auto name = l::Lower(files::Utf8(it->path().filename()));
                    if (l::Lower(files::Utf8(it->path().extension())) != ".iwd")
                        continue;
                    if (name.starts_with("zct"))
                        frameworks.insert("ZCT");
                    else if (name.starts_with("ugx"))
                        frameworks.insert("UGX");
                }
            if (!frameworks.empty())
            {
                std::string list;
                for (const auto& f : frameworks)
                    list += (list.empty() ? "" : ", ") + f;
                Say("map framework: " + list + " (its scripts drive the map, like any map's)");
            }
            _wputenv_s(L"WAW2BO2_VERBOSE", settings.verbose ? L"1" : L"0");
            auto paths = l::BuildPaths::ForSettings(settings);
            if (options.root)
            {
                const auto root = extract::Resolve(*options.root);
                paths = {root, root / "stage", paths.stock, root / "waw_dumps", root / "mod_build", root / "package"};
            }
            const auto waw = l::Path(settings.waw), bo2 = l::Path(settings.bo2);
            const auto t4 = l::Path(settings.t4) / "Unlinker.exe", t6 = l::Path(settings.t6) / "Unlinker.exe";
            const auto wawraw = extract::Ready(waw, t4, caches.waw, "T4");
            const auto stock = extract::Ready(bo2, t6, caches.bo2, "T6");
            const auto& stage = paths.stage;
            const auto& mod = paths.mod;
            const auto& project = settings.project;
            const auto projectRoot = stage / "zone_raw" / l::Path(project);
            if (HasMenuArtwork(settings))
                RunCli({"stage-art", "--settings-json", py::JsonDumpsDefault(settings.ToJson()), "--root", S(projectRoot)});
            else
            {
                SetStep("stage-art");
                StageMenuSettings(settings, projectRoot);
            }
            fs::create_directories(stage);
            fs::create_directories(mod);
            fs::copy_file(stock / "t6api_cache.json", stage / "t6api_cache.json", fs::copy_options::overwrite_existing);
            Say("== 0. reusing shared game assets and preparing cached map sources");
            std::vector<std::pair<std::string, fs::path>> sources;
            {
                timeline::Span span("0 map source dumps");
                sources = extract::SourceDumps({l::Path(settings.fastfile), waw, t4}, caches.waw, options.redump);
            }
            const auto source = extract::Resolve(l::Path(settings.fastfile));
            const auto find = [&](const std::string& role) -> std::optional<fs::path>
            {
                for (const auto& [r, p] : sources)
                    if (r == role)
                        return p;
                return std::nullopt;
            };
            const auto mapraw = *find("map");
            const auto mapzone = WorldName(mapraw, files::Utf8(source.stem()));
            std::vector<std::string> extra;
            for (const auto& [_, p] : sources)
                extra.insert(extra.end(), {"--extra-root", S(p)});
            extra.insert(extra.end(), {"--extra-root", S(wawraw)});
            for (const auto& [_, p] : sources)
                extra.insert(extra.end(), {"--map-root", S(p)});
            const auto linker = l::Path(settings.t6) / "Linker.exe";
            Say("== 1. using pre-extracted stock assets (no stock dumping)");
            Say("== 2. staging " + project);
            std::vector<std::string> args{"stage-bridge", S(stage), project, "--gfx", S(mapraw / "waw2bo2/maps" / l::Path(mapzone + ".d3dbsp.gfx.bin")), "--clip",
                                          S(mapraw / "waw2bo2/maps" / l::Path(mapzone + ".d3dbsp.clip.bin"))};
            args.insert(args.end(), extra.begin(), extra.end());
            const auto scriptRoot = find("mod").value_or(mapraw);
            args.insert(args.end(), {"--iwd-dir", S(source.parent_path()), "--iwd-dir", S(waw / "main"), "--waw-map-script", S(mapraw / "maps" / l::Path(mapzone + ".gsc")),
                                     "--waw-script-root", S(scriptRoot), "--waw-stock-scripts", S(wawraw), "--t6-unlinker", S(t6), "--waw-root", S(waw), "--t4-unlinker",
                                     S(t4), "--waw-stock-dumps", S(caches.waw), "--xwma-decoder", settings.decoder, "--approximate-sound-curves", "--stock-materials",
                                     S(stock / "materials"), "--techset-dump", S(stock), "--bo2", S(bo2), "--script-template", S(stage / "zone_raw/bridge"),
                                     "--script-template-name", "bridge"});
            if (settings.bo2_stock_perks)
                args.push_back("--bo2-stock-perks");
            if (settings.remaster)
                args.push_back("--remaster-bo2-materials");
            if (const auto patch = find("patch"))
                args.insert(args.end(), {"--waw-patch-script-root", S(*patch)});
            {
                const auto relative = source.parent_path().lexically_relative(extract::Resolve(waw / "zone"));
                if (!relative.empty() && *relative.begin() != "..")
                    args.push_back("--waw-stock-map");
            }
            if (settings.source_fx)
                args.insert(args.end(), {"--waw-mod-tools", settings.waw_tools, "--waw-source-fx", "--waw-source-dumps", S(caches.waw / "source")});
            const wchar_t* flac = _wgetenv(L"WAW2BO2_STREAM_FLAC");
            const auto fingerprint = hash::Sha256Hex(py::JsonDumpsDefault(py::Json::array({ConverterDigest(), args, flac && std::wstring(flac) == L"1"})));
            const auto receiptPath = stage / "stage_receipt.json";
            bool reuse = false;
            std::string reason;
            if (settings.resume)
            {
                try
                {
                    const auto receipt = py::Json::parse(files::ReadText(receiptPath));
                    if (receipt.value("project", std::string()) != project)
                        reason = "the last staging was for another map name";
                    else if (receipt.value("fingerprint", std::string()) != fingerprint)
                        reason = "the map sources, build options or converter version changed since the last staging";
                    else if (!fs::is_directory(stage / "zone_raw" / l::Path(project), e))
                        reason = "the staged files are missing";
                    else
                        reuse = true;
                }
                catch (const std::exception&)
                {
                    reason = "the last build did not finish staging";
                }
            }
            if (reuse)
            {
                const auto recorded = py::Json::parse(files::ReadText(receiptPath)).value("files", py::Json::object());
                const auto current = EditableFiles(stage / "zone_raw" / l::Path(project));
                std::vector<std::string> changed, added, removed;
                for (const auto& [name, stamp] : current.items())
                {
                    if (!recorded.contains(name))
                        added.push_back(name);
                    else if (recorded[name] != stamp)
                        changed.push_back(name);
                }
                for (const auto& [name, _] : recorded.items())
                    if (!current.contains(name))
                        removed.push_back(name);
                std::sort(changed.begin(), changed.end());
                std::sort(added.begin(), added.end());
                std::sort(removed.begin(), removed.end());
                changed.insert(changed.end(), added.begin(), added.end());
                Say("== 2. resuming: keeping the staged files of the last build (" + std::to_string(changed.size()) + " edited, " + std::to_string(removed.size())
                    + " removed since staging)");
                changed.insert(changed.end(), removed.begin(), removed.end());
                for (size_t i = 0; i < std::min<size_t>(20, changed.size()); i++)
                    Say("   kept edit: zone_raw/" + project + "/" + changed[i]);
            }
            else
            {
                if (settings.resume)
                    Say("== 2. cannot resume: " + reason + "; staging again from the WaW sources");
                fs::remove(receiptPath, e);
                {
                    timeline::Span span("2 stage-bridge");
                    RunCli(args);
                }
                files::WriteText(receiptPath, py::JsonDumpsDefault(py::Json{{"project", project}, {"fingerprint", fingerprint}, {"files", EditableFiles(stage / "zone_raw" / l::Path(project))}}));
            }
            EnsureLaa(linker);
            Say("== 3a. linking gameplay mod.ff with the BO2 mod tools linker");
            const auto ready = mod / "scripts_ready";
            fs::remove(ready, e);
            // build-mod finalizes the map scripts right after its first mod.ff
            // link (scripts_ready); the map zone compiles, links and is audited
            // meanwhile, and mod.ff is audited once build-mod wrote it
            std::exception_ptr gameplayError;
            std::atomic<bool> gameplayRunning{true};
            const auto parent = timeline::Current();
            std::thread gameplay(
                [&]
                {
                    try
                    {
                        timeline::Parent nest(parent);
                        {
                            timeline::Span span("3a build-mod");
                            SetStep("build-mod");
                            BuildMod({stage, bo2, mod, t6, project, linker, stock, ready});
                        }
                        AuditZone(mod / "out/mod.ff", stage, project, bo2, stock, t6);
                    }
                    catch (...)
                    {
                        gameplayError = std::current_exception();
                    }
                    gameplayRunning = false;
                });
            const auto join = [&]
            {
                if (gameplay.joinable())
                    gameplay.join();
                if (gameplayError)
                    std::rethrow_exception(std::exchange(gameplayError, nullptr));
            };
            try
            {
                while (!IsFile(ready) && gameplayRunning)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!IsFile(ready))
                {
                    join();
                    throw StepError("RuntimeError", "build-mod ended without finishing the map scripts");
                }
                Say("== 3b. compiling map scripts with the BO2 mod tools linker");
                {
                    timeline::Span span("3b compile-scripts");
                    SetStep("compile-scripts");
                    const auto [compiled, log] = CompileScripts(extract::Resolve(stage), project, extract::Resolve(bo2), extract::Resolve(t6));
                    for (const auto& name : compiled)
                        Say("compiled " + name);
                }
                Say("== 4. linking " + project + " with the T6 bridge");
                {
                    timeline::Span span("4 bridge-link");
                    SetStep("bridge-link");
                    const auto log = stage / l::Path(project + "_bridge.log");
                    if (const int code = BridgeLink(stage, project, linker, &stock, &log))
                        throw StepError("RuntimeError", "bridge-link failed (" + std::to_string(code) + ")");
                }
                Say("== 6. auditing built material arguments");
                AuditZone(stage / "zone_out" / l::Path(project) / l::Path(project + ".ff"), stage, project, bo2, stock, t6);
            }
            catch (...)
            {
                if (gameplay.joinable())
                    gameplay.join();
                throw;
            }
            join();
            return 0;
        }

        int LockedBuild(const l::Settings& settings, const BuildOptions& options, const l::CachePaths& caches)
        {
            for (const auto& [game, tool, cache, engine] :
                 {std::tuple{l::Path(settings.waw), l::Path(settings.t4) / "Unlinker.exe", caches.waw, std::string("T4")},
                  std::tuple{l::Path(settings.bo2), l::Path(settings.t6) / "Unlinker.exe", caches.bo2, std::string("T6")}})
                try
                {
                    extract::Ready(game, tool, cache, engine);
                }
                catch (const extract::Error&)
                {
                    Say("== 1. preparing " + engine + " shared game assets automatically");
                    extract::Prepare(game, tool, cache, engine);
                }
            // extraction may prune old generations: protect the views this build reads
            std::vector<fs::path> roots{caches.waw, caches.bo2};
            std::sort(roots.begin(), roots.end(), [](const fs::path& a, const fs::path& b) { return S(a) < S(b); });
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            std::vector<std::unique_ptr<extract::CacheLock>> locks;
            for (const auto& root : roots)
                locks.push_back(std::make_unique<extract::CacheLock>(root));
            SetStep("");
            const double started = Now();
            int result = 0;
            try
            {
                result = DoBuild(settings, options, caches);
            }
            catch (const std::exception& error)
            {
                Remember(settings, options.root, "failed", Describe(error), started);
                throw;
            }
            Remember(settings, options.root, "built", "", started);
            return result;
        }
    } // namespace

    int BuildMap(const l::Settings& settings, const BuildOptions& options)
    {
        const auto caches = options.caches.value_or(l::CachePaths::ForSettings(settings));
        std::string failures;
        for (const auto& c : l::Preflight(settings, true, &caches))
            if (c.required && !c.ready)
                failures += (failures.empty() ? "" : "\n") + c.name + ": " + c.detail;
        if (!failures.empty())
            throw StepError("ValueError", failures);
        const auto stage = options.root ? extract::Resolve(*options.root) / "stage" : l::BuildPaths::ForSettings(settings).stage;
        const bool owned = !timeline::Path();
        if (owned)
            timeline::Start(stage / "build_timeline.jsonl");
        try
        {
            int result;
            {
                timeline::Span span("build", py::Json{{"project", settings.project}});
                result = LockedBuild(settings, options, caches);
            }
            if (owned)
                WriteTimelineSummary(*timeline::Path());
            return result;
        }
        catch (...)
        {
            if (owned)
                WriteTimelineSummary(*timeline::Path());
            throw;
        }
    }
} // namespace w2b::pipeline
