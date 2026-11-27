// Preflight, build paths, the process runner and the build actions (see build.h).
#include "build.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"
#include "core/regex.h"
#include "images.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace w2b::launcher
{
    namespace
    {
        bool Exists(const fs::path& path)
        {
            std::error_code error;
            return fs::exists(path, error);
        }

        bool IsFile(const fs::path& path)
        {
            std::error_code error;
            return fs::is_regular_file(path, error);
        }

        // Path.resolve(): absolute, normalized, links followed where they exist
        fs::path Resolve(const fs::path& path)
        {
            std::error_code error;
            auto absolute = fs::absolute(path.empty() ? fs::current_path(error) : path, error);
            auto resolved = fs::weakly_canonical(absolute, error);
            return error ? absolute.lexically_normal() : resolved;
        }

        std::wstring WideEnv(const wchar_t* name)
        {
            const wchar_t* value = _wgetenv(name);
            return value ? value : L"";
        }

        std::string Strip(const std::string& text)
        {
            const auto first = text.find_first_not_of(" \t\r\n\f\v");
            if (first == std::string::npos)
                return "";
            return text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1);
        }

        std::string Join(const std::vector<std::string>& parts, const std::string& sep)
        {
            std::string out;
            for (size_t i = 0; i < parts.size(); i++)
                out += (i ? sep : "") + parts[i];
            return out;
        }

        // json.dumps(value) of a string, as all2raw.key hashes it
        std::string Key(const std::string& value)
        {
            return hash::Sha256Hex(util::JsonDumpsDefault(util::Json(value))).substr(0, 24);
        }

        std::string InstallKey(const std::string& value)
        {
            return Lower(Utf8(Resolve(Path(value))));
        }

        const std::string WORLD_DUMPER_MARKER = "waw2bo2_primary_lights";

        std::string ExtractorProblem(const fs::path& tool)
        {
            if (!IsFile(tool))
                return "Missing: " + Utf8(tool.filename());
            static std::mutex mutex;
            static std::map<std::pair<std::wstring, uintmax_t>, bool> cache;
            std::error_code error;
            const auto size = fs::file_size(tool, error);
            const auto key = std::make_pair(Resolve(tool).wstring(), size);
            std::lock_guard lock(mutex);
            auto found = cache.find(key);
            if (found == cache.end())
            {
                std::string bytes;
                files::ReadBytes(tool, bytes);
                found = cache.emplace(key, bytes.find(WORLD_DUMPER_MARKER) != std::string::npos).first;
            }
            if (found->second)
                return "";
            return Utf8(tool) + " is not the converter's WaW extractor: it cannot export map worlds. "
                   "Clear \"WaW extractor folder\" in Setup > Advanced Paths and click Auto-detect to use the bundled one.";
        }

        std::string FormatG(const double value)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%g", value);
            return buffer;
        }

        void Coordinate(const std::string& value, const std::string& label, const double limit)
        {
            double number = 0;
            if (!util::ParseFloat(value, number) || !std::isfinite(number) || std::fabs(number) > limit)
                throw std::invalid_argument(label + " must be a number between " + FormatG(-limit) + " and " + FormatG(limit) + ".");
        }

        // menuart.validate_art + mapmenu.validate_settings
        void ValidateArtwork(const Settings& s)
        {
            const std::string* paths[] = {&s.menu_blit, &s.menu_large, &s.menu_blur};
            if (!s.menu_blit.empty() || !s.menu_large.empty() || !s.menu_blur.empty())
            {
                for (const auto* p : paths)
                    if (p->empty())
                        throw std::invalid_argument("Choose all three images: Blit, Large, and Blur.");
                const auto blit = DecodeImage(Path(s.menu_blit));
                if (blit.MinAlpha() == 255)
                    throw std::invalid_argument("Blit needs a transparent background. Choose a PNG or TGA with alpha transparency.");
                DecodeImage(Path(s.menu_large));
                DecodeImage(Path(s.menu_blur));
            }
            Coordinate(s.menu_longitude, "Longitude", 180);
            Coordinate(s.menu_latitude, "Latitude", 90);
            if (!s.loading_song.empty())
            {
                if (("mus_load_" + s.project + "_patch").size() >= 64)
                    throw std::invalid_argument("Loading music requires a BO2 map name of at most 48 characters.");
                std::error_code error;
                const auto source = Path(s.loading_song);
                if (!IsFile(source) || fs::file_size(source, error) == 0)
                    throw std::invalid_argument("Choose an existing, nonempty loading-song audio file.");
            }
        }

        long long MtimeNs(const fs::path& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                return 0;
            ULARGE_INTEGER t;
            t.LowPart = data.ftLastWriteTime.dwLowDateTime;
            t.HighPart = data.ftLastWriteTime.dwHighDateTime;
            return (static_cast<long long>(t.QuadPart) - 116444736000000000LL) * 100;
        }

        std::vector<fs::path> Glob(const fs::path& folder, const std::string& extension)
        {
            std::vector<fs::path> found;
            std::error_code error;
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
                if (Lower(Utf8(it->path().extension())) == extension)
                    found.push_back(it->path());
            return found;
        }

        fs::path LocalAppData()
        {
            const auto local = WideEnv(L"LOCALAPPDATA");
            return local.empty() ? fs::path(WideEnv(L"USERPROFILE")) : fs::path(local);
        }

        std::wstring Widen(const std::string& text)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        void Fail(const std::string& message)
        {
            throw std::runtime_error(message);
        }
    } // namespace

    fs::path PlutoniumRoot()
    {
        return LocalAppData() / "Plutonium";
    }

    CachePaths CachePaths::ForSettings(const Settings& settings)
    {
        const auto base = Resolve(Path(settings.work)) / "asset_cache";
        return {base / Path("waw_" + Key(InstallKey(settings.waw))), base / Path("bo2_" + Key(InstallKey(settings.bo2)))};
    }

    BuildPaths BuildPaths::ForSettings(const Settings& settings)
    {
        if (const auto problem = ValidateProject(settings.project); !problem.empty())
            throw std::invalid_argument(problem);
        const auto identity = Lower(Utf8(Resolve(Path(settings.fastfile))));
        const auto digest = hash::Sha256Hex(identity).substr(0, 8);
        const auto gameKey = hash::Sha256Hex(Lower(settings.waw) + std::string(1, '\0') + Lower(settings.bo2)).substr(0, 8);
        const auto root = Resolve(Path(settings.work)) / Path(settings.project + "_" + digest + "_" + gameKey);
        return {root, root / "stage", CachePaths::ForSettings(settings).bo2 / "views", root / "waw_dumps", root / "mod_build", root / "package"};
    }

    bool BuildPaths::Complete(const std::string& project, const bool* bo2StockPerks) const
    {
        try
        {
            const auto receipt = util::Json::parse(files::ReadText(root / "build.json"));
            if (bo2StockPerks && receipt.value("bo2_stock_perks", false) != *bo2StockPerks)
                return false;
            if (receipt.value("project", std::string()) != project)
                return false;
            for (const auto& name : {project + ".ff", project + ".ipak", std::string("mod.ff"), std::string("mod_load.ff"), std::string("mod.json")})
                if (!IsFile(output / Path(name)))
                    return false;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    std::vector<Check> Preflight(const Settings& settings, const bool includeMap, const CachePaths* cacheOverride)
    {
        std::vector<Check> checks;
        const auto files = [&](const std::string& name, const std::string& folder, const std::vector<std::string>& paths, const bool required = true)
        {
            std::vector<std::string> missing;
            for (const auto& p : paths)
                if (folder.empty() || !Exists(Path(folder) / Path(p)))
                    missing.push_back(p);
            checks.push_back({name, missing.empty(), missing.empty() ? "Ready" : "Missing: " + Join(missing, ", "), required});
        };
        files("World at War", settings.waw, {"main", "zone/english/common.ff", "zone/english/code_post_gfx.ff"});
        files("Black Ops II + Mod Tools", settings.bo2,
              {"bin/Linker.exe", "raw/animtrees/fxanim_props.atr", "raw/zm/mapstable.csv", "raw/zm/gametypestable.csv", "raw/maps/mp/zombies/_zm.gsc",
               "mods/zm_test/maps/mp/zm_test.gsc", "mods/zm_test/clientscripts/mp/zm_test.csc", "zone/all/common_zm.ff", "zone/all/code_post_gfx_zm.ff"});
        const auto problem = settings.t4.empty() ? std::string("Missing: Unlinker.exe") : ExtractorProblem(Path(settings.t4) / "Unlinker.exe");
        checks.push_back({"WaW extractor", problem.empty(), problem.empty() ? "Ready" : problem});
        files("BO2 bridge tools", settings.t6, {"Unlinker.exe", "Linker.exe"});
        const bool decoder = !settings.decoder.empty() && IsFile(Path(settings.decoder));
        checks.push_back({"Audio decoder", decoder, decoder ? "Ready" : "Select xaudio_wma_decoder.exe in Advanced paths."});
        files("WaW source tools", settings.waw_tools, {"bin/linker_pc.exe"}, settings.source_fx);
        if (includeMap)
        {
            const auto caches = cacheOverride ? *cacheOverride : CachePaths::ForSettings(settings);
            const bool extracted = IsFile(caches.waw / "all2raw.json") && IsFile(caches.bo2 / "all2raw.json");
            checks.push_back({"Shared game assets", extracted,
                              extracted ? "Verified and refreshed automatically when building" : "Build Map will prepare the shared game assets automatically", false});
            const auto source = Path(settings.fastfile);
            const bool valid = !settings.fastfile.empty() && IsFile(source) && Lower(Utf8(source.extension())) == ".ff";
            checks.push_back({"Source map", valid, valid ? Utf8(source.filename()) : "Choose the source map fastfile (.ff)."});
            if (valid)
            {
                files("Companion mod.ff", Utf8(source.parent_path()), {"mod.ff"}, false);
                static const regex::Regex stem(R"([A-Za-z0-9_\-]+)");
                const bool validStem = stem.FullMatch(Utf8(source.stem())).has_value();
                checks.push_back({"Source zone name", validStem, validStem ? "Ready" : "Source fastfile name must use letters, numbers, underscores, or hyphens."});
            }
            const auto projectProblem = ValidateProject(settings.project);
            checks.push_back({"BO2 map name", projectProblem.empty(), projectProblem.empty() ? settings.project : projectProblem});
            try
            {
                ValidateArtwork(settings);
                checks.push_back({"Map artwork", true, "Ready"});
            }
            catch (const std::exception& error)
            {
                checks.push_back({"Map artwork", false, error.what()});
            }
        }
        const bool work = !Strip(settings.work).empty();
        checks.push_back({"Build folder", work, work ? settings.work : "Choose a writable build folder."});
        return checks;
    }

    std::vector<std::string> NativeCommand(const std::vector<std::string>& args)
    {
        wchar_t buffer[MAX_PATH * 4];
        const auto length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        auto exe = fs::path(std::wstring(buffer, length)).parent_path() / "waw2bo2n.exe";
        if (!IsFile(exe))
            exe = ResourceRoot() / "native" / "build" / "waw2bo2n.exe";
        std::vector<std::string> command{Utf8(exe)};
        command.insert(command.end(), args.begin(), args.end());
        return command;
    }

    std::vector<std::string> BuildCommand(const Settings& settings)
    {
        BuildPaths::ForSettings(settings);
        // the native build driver (pipeline/driver.cpp); it runs the steps still in
        // the conversion worker steps itself
        auto command = NativeCommand({"build-map", "--settings-json", util::JsonDumpsDefault(settings.ToJson())});
        if (settings.redump)
            command.push_back("--redump");
        return command;
    }

    // ---------------------------------------------------------------- ProcessRunner

    ProcessRunner::ProcessRunner(Emit emit, const bool verbose, const int heartbeatSeconds) : emit(std::move(emit)), verbose(verbose), heartbeatSeconds(heartbeatSeconds) {}

    ProcessRunner::~ProcessRunner()
    {
        if (job)
            CloseHandle(job);
    }

    void ProcessRunner::Cancel()
    {
        cancelled = true;
        std::lock_guard lock(mutex);
        if (job)
            TerminateJobObject(job, 1);
    }

    int ProcessRunner::Run(const std::vector<std::string>& command, const fs::path& log)
    {
        if (cancelled)
            return -1;
        fs::create_directories(log.parent_path());
        std::ofstream output(log, std::ios::binary | std::ios::trunc);
        std::mutex writeMutex;
        auto lastOutput = std::chrono::steady_clock::now();
        const auto started = lastOutput;
        std::string lastLine = "waiting for worker output";
        const auto write = [&](const std::string& line)
        {
            std::lock_guard lock(writeMutex);
            output << line << "\r\n";
            output.flush();
            emit(line);
        };

        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &inherit, 0))
            Fail("cannot create the worker pipe");
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW startup{sizeof startup};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nul;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;

        // the environment, with the worker's settings
        std::map<std::wstring, std::wstring, std::less<>> variables;
        if (wchar_t* block = GetEnvironmentStringsW())
        {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
            {
                const std::wstring entry(p);
                const auto eq = entry.find(L'=', 1);
                if (eq != std::wstring::npos)
                    variables[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
            FreeEnvironmentStringsW(block);
        }
        const auto root = ResourceRoot();
        variables[L"WAW2BO2_VERBOSE"] = verbose ? L"1" : L"0";
        std::wstring environment;
        for (const auto& [k, v] : variables)
            environment += k + L"=" + v + L'\0';
        environment += L'\0';

        auto line = Widen(process::CommandLine(command));
        PROCESS_INFORMATION info{};
        const auto cwd = root.wstring();
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                            cwd.c_str(), &startup, &info))
        {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            CloseHandle(nul);
            Fail("cannot start " + command.front() + " (error " + std::to_string(GetLastError()) + ")");
        }
        {
            std::lock_guard lock(mutex);
            if (job)
                CloseHandle(job);
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            AssignProcessToJobObject(job, info.hProcess);
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        CloseHandle(writePipe);
        CloseHandle(nul);
        if (cancelled)
            Cancel();

        std::mutex heartbeatMutex;
        std::condition_variable heartbeatWake;
        bool finished = false;
        std::thread heartbeat(
            [&]
            {
                std::unique_lock lock(heartbeatMutex);
                while (!heartbeatWake.wait_for(lock, std::chrono::seconds(heartbeatSeconds), [&] { return finished; }))
                {
                    const auto now = std::chrono::steady_clock::now();
                    std::string last;
                    {
                        std::lock_guard w(writeMutex);
                        last = lastLine.substr(0, 240);
                    }
                    const auto quiet = std::chrono::duration_cast<std::chrono::seconds>(now - lastOutput).count();
                    if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT && quiet >= heartbeatSeconds)
                    {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
                        write("[activity] Process " + std::to_string(info.dwProcessId) + " still running; elapsed " + std::to_string(elapsed / 60) + "m "
                              + std::to_string(elapsed % 60) + "s; no new output for " + std::to_string(quiet) + "s. Last output: " + last);
                    }
                }
            });

        std::string pending;
        char buffer[8192];
        DWORD read = 0;
        const auto flush = [&](std::string raw)
        {
            auto text = encoding::Utf8Replace(raw);
            text.erase(text.find_last_not_of(" \t\r\n\f\v") + 1 > text.size() ? text.size() : text.find_last_not_of(" \t\r\n\f\v") + 1);
            if (text.find_first_not_of(" \t\r\n\f\v") == std::string::npos)
                text.clear();
            lastOutput = std::chrono::steady_clock::now();
            {
                std::lock_guard w(writeMutex);
                lastLine = text;
            }
            write(text);
        };
        while (ReadFile(readPipe, buffer, sizeof buffer, &read, nullptr) && read)
        {
            pending.append(buffer, read);
            size_t start = 0;
            for (size_t i = 0; i < pending.size(); i++)
            {
                if (pending[i] != '\n' && pending[i] != '\r')
                    continue;
                if (pending[i] == '\r' && i + 1 == pending.size())
                    break; // a "\r\n" may continue in the next read
                flush(pending.substr(start, i - start));
                if (pending[i] == '\r' && pending[i + 1] == '\n')
                    i++;
                start = i + 1;
            }
            pending.erase(0, start);
        }
        if (!pending.empty())
            flush(pending);
        CloseHandle(readPipe);
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        {
            std::lock_guard lock(heartbeatMutex);
            finished = true;
        }
        heartbeatWake.notify_all();
        heartbeat.join();
        return static_cast<int>(code);
    }

    // ---------------------------------------------------------------- flows

    util::Json InputStamp(const Settings& settings)
    {
        std::vector<fs::path> paths;
        const auto source = Path(settings.fastfile).parent_path();
        for (const auto* ext : {".ff", ".iwd"})
            for (const auto& p : Glob(source, ext))
                paths.push_back(p);
        for (const auto& p : Glob(Path(settings.waw) / "main", ".iwd"))
            paths.push_back(p);
        for (const auto& p : Glob(Path(settings.waw) / "zone/english", ".ff"))
            paths.push_back(p);
        for (const auto& p : Glob(Path(settings.bo2) / "zone/all", ".ff"))
            paths.push_back(p);
        paths.push_back(Path(settings.t4) / "Unlinker.exe");
        paths.push_back(Path(settings.t6) / "Unlinker.exe");
        const auto key = [](const fs::path& p)
        {
            std::vector<std::string> parts;
            for (const auto& part : p)
                parts.push_back(Lower(Utf8(part)));
            return parts;
        };
        std::sort(paths.begin(), paths.end(), [&](const fs::path& a, const fs::path& b) { return key(a) < key(b); });
        paths.erase(std::unique(paths.begin(), paths.end(), [&](const fs::path& a, const fs::path& b) { return key(a) == key(b); }), paths.end());
        util::Json stamp = util::Json::object();
        for (const auto& p : paths)
            if (IsFile(p))
            {
                std::error_code error;
                stamp[Utf8(Resolve(p))] = util::Json::array({fs::file_size(p, error), MtimeNs(p)});
            }
        return stamp;
    }

    int PerformBuild(const Settings& settings, ProcessRunner& runner)
    {
        std::vector<std::string> failures;
        for (const auto& c : Preflight(settings))
            if (c.required && !c.ready)
                failures.push_back(c.name + ": " + c.detail);
        if (!failures.empty())
            Fail(Join(failures, "\n"));
        const auto paths = BuildPaths::ForSettings(settings);
        fs::create_directories(paths.stage);
        // an OS lock through build and package keeps other launchers out
        const HANDLE lock = CreateFileW((paths.root / "build.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock == INVALID_HANDLE_VALUE)
            Fail("cannot open " + Utf8(paths.root / "build.lock"));
        struct Close
        {
            HANDLE h;
            ~Close()
            {
                CloseHandle(h);
            }
        } closer{lock};
        DWORD written = 0;
        SetFilePointer(lock, 0, nullptr, FILE_END);
        WriteFile(lock, "0", 1, &written, nullptr);
        OVERLAPPED at{};
        if (!LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at))
            Fail("This map is already building in another launcher.");
        const auto receipt = paths.root / "build.json";
        std::error_code error;
        fs::remove(receipt, error);
        int code = runner.Run(BuildCommand(settings), paths.root / "build.log");
        if (code || runner.Cancelled())
            return code ? code : -1;
        std::vector<std::string> missing;
        for (const auto& p : {paths.stage / "zone_out" / Path(settings.project) / Path(settings.project + ".ff"),
                              paths.stage / "zone_out" / Path(settings.project) / Path(settings.project + ".ipak"), paths.mod / "out" / "mod.ff",
                              paths.mod / "out" / "mod_load.ff"})
            if (!IsFile(p))
                missing.push_back(Utf8(p));
        if (!missing.empty())
            Fail("Build driver returned success without creating the required map files. Review " + Utf8(paths.root / "build.log")
                 + ". If that log is empty, the bundled worker did not execute the build pipeline. Missing: " + Join(missing, ", "));
        runner.Line("== 7. assembling the finished map package");
        code = runner.Run(NativeCommand({"package", Utf8(paths.stage), settings.project, "--bo2", settings.bo2, "--work", Utf8(paths.mod), "--dest", Utf8(paths.output)}),
                          paths.root / "package.log");
        if (code || runner.Cancelled())
            return code ? code : -1;
        for (const auto& name : {settings.project + ".ff", settings.project + ".ipak", std::string("mod.ff"), std::string("mod_load.ff"), std::string("mod.json")})
            if (!IsFile(paths.output / Path(name)))
                Fail("Packaging did not create all required map files. Review package.log.");
        util::Json data = util::Json::object();
        data["project"] = settings.project;
        data["source"] = settings.fastfile;
        data["output"] = Utf8(paths.output);
        data["bo2_stock_perks"] = settings.bo2_stock_perks;
        files::WriteText(receipt, util::JsonDumps(data, 2));
        files::WriteText(paths.root / "cache.json", util::JsonDumpsDefault(InputStamp(settings)));
        fs::remove(paths.root / "installed.json", error);
        return 0;
    }

    int PerformExtractAll(const Settings& settings, ProcessRunner& runner)
    {
        std::vector<std::string> failures;
        for (const auto& c : Preflight(settings, false))
            if (c.required && !c.ready && c.name != "Audio decoder" && c.name != "WaW source tools")
                failures.push_back(c.name + ": " + c.detail);
        if (!failures.empty())
            Fail(Join(failures, "\n"));
        return runner.Run(NativeCommand({"all2raw", "--settings-json", util::JsonDumpsDefault(settings.ToJson())}), Path(settings.work) / "asset_cache/extract.log");
    }

    int PerformInstall(const Settings& settings, ProcessRunner& runner)
    {
        const auto paths = BuildPaths::ForSettings(settings);
        if (!paths.Complete(settings.project, &settings.bo2_stock_perks))
            return 0;
        const int code = runner.Run(NativeCommand({"package", Utf8(paths.stage), settings.project, "--bo2", settings.bo2, "--work", Utf8(paths.mod)}), paths.root / "install.log");
        if (!code && !runner.Cancelled())
        {
            util::Json data = util::Json::object();
            data["output"] = Utf8(paths.output);
            files::WriteText(paths.root / "installed.json", util::JsonDumpsDefault(data));
        }
        return code;
    }

    bool Installed(const Settings& settings)
    {
        if (settings.project.empty() || settings.bo2.empty())
            return false;
        const auto folder = PlutoniumRoot() / "storage/t6/mods" / Path(settings.project);
        try
        {
            const auto paths = BuildPaths::ForSettings(settings);
            const auto receipt = util::Json::parse(files::ReadText(paths.root / "installed.json"));
            if (receipt.value("output", std::string()) != Utf8(paths.output))
                return false;
            for (const auto& name : {settings.project + ".ff", settings.project + ".ipak", std::string("mod.ff"), std::string("mod_load.ff"), std::string("mod.json")})
                if (!IsFile(folder / Path(name)))
                    return false;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void LaunchMap(const Settings& settings)
    {
        const auto pluto = PlutoniumRoot();
        const auto boot = pluto / "bin/plutonium-bootstrapper-win32.exe";
        if (!Installed(settings) || !IsFile(boot))
            Fail("Install the built map and configure Plutonium T6 first.");
        auto line = Widen(process::CommandLine({Utf8(boot), "t6zm", settings.bo2, "-lan", "+set", "fs_game", "mods/" + settings.project,
                                                           // the game's console log in the mod folder, for Save Diagnostics
                                                           "+set", "logfile", "2", "+devmap", settings.project}));
        STARTUPINFOW startup{sizeof startup};
        PROCESS_INFORMATION info{};
        const auto cwd = pluto.wstring();
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup, &info))
            Fail("Launch failed (error " + std::to_string(GetLastError()) + ")");
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
} // namespace w2b::launcher
