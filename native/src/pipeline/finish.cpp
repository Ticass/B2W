// Final build steps (see finish.h).
#include "finish.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"
#include "core/pyre.h"
#include "core/pytext.h"
#include "core/timeline.h"
#include "gsc/parse.h"
#include "gsc/port/generators.h"
#include "gsc/port/staging.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <set>

#ifndef W2B_VERSION
#define W2B_VERSION "0.0.0"
#endif

namespace w2b::pipeline
{
    namespace
    {
        // str(Path): Windows separators
        std::string Str(fs::path path)
        {
            return files::Utf8(path.make_preferred());
        }

        fs::path Resolve(const fs::path& path)
        {
            std::error_code error;
            const auto absolute = fs::absolute(path, error);
            const auto resolved = fs::weakly_canonical(absolute, error);
            return error ? absolute.lexically_normal() : resolved;
        }

        bool IsFile(const fs::path& path)
        {
            std::error_code error;
            return fs::is_regular_file(path, error);
        }

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        // sorted(Path...) on Windows: parts compared case-insensitively
        void SortPaths(std::vector<fs::path>& paths)
        {
            const auto key = [](const fs::path& p)
            {
                std::vector<std::wstring> parts;
                for (const auto& part : p)
                {
                    auto text = part.wstring();
                    for (auto& c : text)
                        c = static_cast<wchar_t>(std::towlower(c));
                    parts.push_back(text);
                }
                return parts;
            };
            std::stable_sort(paths.begin(), paths.end(), [&](const fs::path& a, const fs::path& b) { return key(a) < key(b); });
        }

        // Path.glob(prefix*suffix) in one folder, directory order
        std::vector<fs::path> Glob(const fs::path& folder, const std::string& prefix, const std::string& suffix)
        {
            std::vector<fs::path> found;
            std::error_code error;
            for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            {
                const auto name = Lower(files::Utf8(it->path().filename()));
                if (name.size() >= prefix.size() + suffix.size() && name.starts_with(Lower(prefix)) && name.ends_with(Lower(suffix)))
                    found.push_back(it->path());
            }
            return found;
        }

        // equal SHA-256 digests, as package.py compares them: equal bytes
        bool SameBytes(const fs::path& a, const fs::path& b)
        {
            std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
            std::vector<char> bx(1 << 20), by(1 << 20);
            while (x && y)
            {
                x.read(bx.data(), static_cast<std::streamsize>(bx.size()));
                y.read(by.data(), static_cast<std::streamsize>(by.size()));
                if (x.gcount() != y.gcount() || !std::equal(bx.begin(), bx.begin() + x.gcount(), by.begin()))
                    return false;
            }
            return x.eof() && y.eof();
        }

        // text=True, errors="replace": UTF-8 with replacement, universal newlines
        std::string Text(const std::string& bytes)
        {
            return encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
        }

        std::string PyList(const std::vector<std::string>& items)
        {
            std::string out = "[";
            for (size_t i = 0; i < items.size(); i++)
                out += (i ? ", " : "") + gsc::Repr(items[i]);
            return out + "]";
        }

        std::vector<std::string> SplitLines(const std::string& text)
        {
            std::vector<std::string> lines;
            for (const auto line : py::SplitLines(text))
                lines.emplace_back(line);
            return lines;
        }

        void Verbose(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }

    } // namespace

    process::Result Captured(const std::vector<std::string>& command, const std::string& cwd)
    {
        const char* flag = std::getenv("WAW2BO2_VERBOSE");
        const bool verbose = flag && std::string(flag) == "1";
        const auto tool = files::Utf8(fs::path(files::FromUtf8(command[0])).filename());
        if (verbose)
            std::printf("[tool] Running %s\n", tool.c_str());
        // each tool run is a span of the build timeline, named by its command line
        std::string name = tool;
        for (size_t i = 1; i < command.size() && name.size() < 160; i++)
            name += " " + command[i];
        const timeline::Span span(name);
        return process::RunMerged(command, cwd, verbose ? Verbose : nullptr);
    }

    std::string QuotedJoin(const std::vector<std::string>& command)
    {
        std::string out;
        for (size_t i = 0; i < command.size(); i++)
            out += (i ? " " : "") + (command[i].find(' ') != std::string::npos ? "\"" + command[i] + "\"" : command[i]);
        return out;
    }

    std::vector<std::string> MapScripts(const fs::path& projectRoot, const std::string& project)
    {
        return gsc::port::MapScripts(projectRoot, project);
    }

    void ValidateOwnedInitCalls(const fs::path& projectRoot, const std::string& project)
    {
        for (const auto& [side, ext] : {std::pair<std::string, std::string>{"maps", "gsc"}, {"clientscripts", "csc"}})
        {
            const auto main = projectRoot / side / "mp" / files::FromUtf8(project + "." + ext);
            if (!IsFile(main))
                continue;
            const auto script = gsc::Parse(files::ReadText(main), Str(main));
            for (const auto& ref : gsc::References(script.tokens))
            {
                if (Lower(ref.name) != "init" || !ref.qualifier || !ref.qualifier->starts_with(side + "\\mp\\waw\\"))
                    continue;
                auto relative = *ref.qualifier;
                std::replace(relative.begin(), relative.end(), '\\', '/');
                const auto target = projectRoot / files::FromUtf8(relative + "." + ext);
                if (!IsFile(target) || !gsc::Parse(files::ReadText(target), Str(target)).Find("init"))
                    throw StepError("StageError", Str(main) + ": generated initializer " + *ref.qualifier + "::init is missing");
            }
        }
    }

    std::pair<std::vector<std::string>, std::vector<std::string>> ParseScriptLinkerLog(const std::string& log)
    {
        static const pyre::Regex bspError(R"re(ERROR: Could not open BSP "[^"\n]*" for map "[^"\n]*"\r?\n?)re");
        static const pyre::Regex compiledLine(R"re(Compiled GSC script "([^"]+)")re");
        const auto clean = bspError.Sub("", log);
        std::set<std::string> compiled;
        for (const auto& row : compiledLine.FindAll(clean))
            compiled.insert(row[0]);
        std::vector<std::string> errors;
        for (const auto& line : SplitLines(clean))
            if (line.find("ERROR") != std::string::npos)
                errors.push_back(line);
        return {std::vector<std::string>(compiled.begin(), compiled.end()), errors};
    }

    std::pair<std::vector<std::string>, std::string> CompileScripts(const fs::path& stage, const std::string& project, const fs::path& bo2, const fs::path& unlinker)
    {
        const auto projectRoot = stage / "zone_raw" / files::FromUtf8(project);
        gsc::port::EnsureOwnedClientBootstrap(projectRoot, bo2);
        ValidateOwnedInitCalls(projectRoot, project);
        const auto scripts = MapScripts(projectRoot, project);
        const auto work = stage / "script_build";
        std::error_code error;
        fs::remove_all(work, error);
        fs::create_directories(work / "zone_source");
        const auto zone = project + "_scripts";
        std::string zoneText = ">game,T6\n";
        for (const auto& s : scripts)
            zoneText += "script," + s + "\n";
        files::WriteText(work / "zone_source" / files::FromUtf8(zone + ".zone"), zoneText);
        const auto linker = bo2 / "bin" / "Linker.exe";
        const auto proc = Captured({Str(linker), "--no-color", "--source-search-path", Str(work / "zone_source"), "--add-asset-search-path", Str(projectRoot),
                                    "--output-folder", Str(work / "out"), zone},
                                   Str(linker.parent_path()));
        const auto log = Text(proc.out);
        files::WriteText(work / "linker.log", log);
        const auto [compiled, errors] = ParseScriptLinkerLog(log);
        std::vector<std::string> missing;
        const std::set<std::string> done(compiled.begin(), compiled.end());
        for (const auto& s : std::set<std::string>(scripts.begin(), scripts.end()))
            if (!done.count(s))
                missing.push_back(s);
        const auto ff = work / "out" / files::FromUtf8(zone + ".ff");
        if (!errors.empty() || !missing.empty() || !IsFile(ff))
        {
            const std::vector<std::string> firstErrors(errors.begin(), errors.begin() + static_cast<long long>(std::min<size_t>(5, errors.size())));
            const std::vector<std::string> firstMissing(missing.begin(), missing.begin() + static_cast<long long>(std::min<size_t>(5, missing.size())));
            throw StepError("StageError", "script compilation failed: " + PyList(firstErrors) + " missing=" + PyList(firstMissing) + "; see " + Str(work / "linker.log"));
        }
        const auto out = stage / "script_build" / "compiled";
        const auto unlink = Captured({Str(unlinker), "--no-color", "--include-assets", "script", "--output-folder", Str(out), Str(ff)});
        const auto unlinkText = Text(unlink.out);
        for (const auto& name : compiled)
        {
            const auto blob = out / files::FromUtf8(name);
            std::string bytes;
            if (unlink.exitCode || !files::ReadBytes(blob, bytes))
                throw StepError("StageError", "compiled script " + name + " was not extracted: " + unlinkText.substr(unlinkText.size() > 400 ? unlinkText.size() - 400 : 0));
            if (bytes.compare(0, 2, "//") == 0 || bytes.substr(0, 200).find("#include") != std::string::npos)
                throw StepError("StageError", "extracted " + name + " is still GSC source, not bytecode");
        }
        return {compiled, log};
    }

    std::vector<std::string> LinkerCommand(const fs::path& linker, const fs::path& stage, const std::string& project, const fs::path* techsetDump)
    {
        std::vector<fs::path> roots;
        const auto compiled = stage / "script_build" / "compiled";
        std::error_code error;
        if (fs::exists(compiled, error))
            roots.push_back(compiled);
        roots.push_back(stage / "zone_raw" / files::FromUtf8(project));
        std::vector<std::string> named;
        for (const auto& r : roots)
        {
            const auto relative = r.lexically_relative(stage);
            named.push_back(!relative.empty() && *relative.begin() != ".." ? "?base?\\" + Str(relative) : Str(r));
        }
        if (techsetDump)
            named.push_back(Str(*techsetDump));
        std::string joined;
        for (size_t i = 0; i < named.size(); i++)
            joined += (i ? ";" : "") + named[i];
        return {Str(linker), "--no-color", "--base-folder", Str(stage), "--asset-search-path", joined, "--source-search-path", Str(stage / "zone_source"),
                "--output-folder", Str(stage / "zone_out" / files::FromUtf8(project)), project};
    }

    int BridgeLink(const fs::path& stageArg, const std::string& project, const fs::path& linkerArg, const fs::path* techsetArg, const fs::path* logArg)
    {
        const auto stage = Resolve(stageArg);
        const auto techset = techsetArg ? std::optional<fs::path>(Resolve(*techsetArg)) : std::nullopt;
        const auto command = LinkerCommand(Resolve(linkerArg), stage, project, techset ? &*techset : nullptr);
        const auto logPath = logArg ? *logArg : stage / files::FromUtf8(project + "_bridge.log");
        std::printf("%s\n", QuotedJoin(command).c_str());
        std::fflush(stdout);
        // KNOWN ISSUE: the bridge linker crashes nondeterministically (access
        // violation); retry a bounded number of times and report every crash.
        process::Result proc;
        for (int attempt = 1; attempt <= 3; attempt++)
        {
            proc = Captured(command, Str(stage));
            if (static_cast<unsigned>(proc.exitCode) != 0xC0000005u)
                break;
            std::fprintf(stderr, "bridge linker crashed (access violation) on attempt %d; retrying\n", attempt);
        }
        const auto text = Text(proc.out);
        files::WriteText(logPath, text);
        const auto lines = SplitLines(text);
        std::set<std::string> missingSet;
        std::vector<std::string> failed;
        for (const auto& l : lines)
        {
            if (l.starts_with("Missing asset"))
                missingSet.insert(l);
            if (l.find("has failed") != std::string::npos || l.starts_with("ERROR") || l.find("Failed to") != std::string::npos)
                failed.push_back(l);
        }
        const std::vector<std::string> missing(missingSet.begin(), missingSet.end());
        for (size_t i = 0; i < std::min<size_t>(30, missing.size()); i++)
            std::printf("%s\n", missing[i].c_str());
        for (size_t i = 0; i < std::min<size_t>(30, failed.size()); i++)
            std::printf("%s\n", failed[i].c_str());
        const auto ff = stage / "zone_out" / files::FromUtf8(project) / files::FromUtf8(project + ".ff");
        const long long code = static_cast<unsigned>(proc.exitCode);
        std::printf("linker exit code %lld; %zu distinct missing assets; log: %s\n", code, missing.size(), Str(logPath).c_str());
        if (proc.exitCode || !missing.empty() || !failed.empty() || !IsFile(ff))
        {
            std::fflush(stdout);
            std::fprintf(stderr, "bridge link NOT successful\n");
            return proc.exitCode ? proc.exitCode : 1;
        }
        std::printf("bridge fastfile: %s\n", Str(ff).c_str());
        return 0;
    }

    fs::path PlutoniumModsDir()
    {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        if (!local)
            throw StepError("KeyError", "'LOCALAPPDATA'");
        return fs::path(local) / "Plutonium" / "storage" / "t6" / "mods";
    }

    fs::path Package(const std::string& project, const fs::path& mapOut, const fs::path& modOut, const fs::path* destArg)
    {
        const auto dest = destArg ? *destArg : PlutoniumModsDir() / files::FromUtf8(project);
        std::vector<fs::path> list{mapOut / files::FromUtf8(project + ".ff"), mapOut / files::FromUtf8(project + ".ipak"), modOut / "mod.ff", modOut / "mod_load.ff"};
        // the template's BO2 bank is not shipped (modzone.TEMPLATE_SOUNDBANK)
        const std::set<std::string> templateBanks{"zmb_test.all.sabl", "zmb_test.all.sabs"};
        std::error_code error;
        if (fs::exists(modOut / "sound", error))
        {
            std::vector<fs::path> banks;
            for (const auto& p : Glob(modOut / "sound", "", ".sabl"))
                banks.push_back(p);
            for (const auto& p : Glob(modOut / "sound", "", ".sabs"))
                banks.push_back(p);
            SortPaths(banks);
            for (const auto& p : banks)
                if (!templateBanks.count(files::Utf8(p.filename())))
                    list.push_back(p);
        }
        for (const auto& name : templateBanks)
            fs::remove(dest / name, error);
        auto packs = Glob(modOut, "", ".ipak");
        SortPaths(packs);
        list.insert(list.end(), packs.begin(), packs.end());
        std::vector<std::string> missing;
        for (const auto& f : list)
            if (!fs::exists(f, error))
                missing.push_back("WindowsPath(" + gsc::Repr(Str(f)) + ")");
        if (!missing.empty())
        {
            std::string text;
            for (size_t i = 0; i < missing.size(); i++)
                text += (i ? ", " : "") + missing[i];
            throw StepError("FileNotFoundError", "cannot package, missing: [" + text + "]");
        }
        fs::create_directories(dest);
        for (const auto& f : list)
        {
            const auto target = dest / f.filename();
            // A running map keeps its image packs open: identical files stay
            if (IsFile(target) && fs::file_size(target, error) == fs::file_size(f, error) && SameBytes(f, target))
                continue;
            fs::copy_file(f, target, fs::copy_options::overwrite_existing);
            fs::last_write_time(target, fs::last_write_time(f, error), error);
        }
        py::Json metadata = py::Json::object();
        if (const auto path = modOut / "menu_metadata.json"; IsFile(path))
            metadata = py::Json::parse(files::ReadText(path));
        py::Json mod = py::Json::object();
        mod["name"] = metadata.contains("title") ? metadata["title"] : py::Json(project + " (B2W)");
        mod["description"] = metadata.contains("description") ? metadata["description"] : py::Json(project + ": World at War custom map converted by waw2bo2");
        mod["version"] = W2B_VERSION;
        files::WriteText(dest / "mod.json", py::JsonDumps(mod, 2) + "\n");
        return dest;
    }

    std::vector<std::string> LaunchCommand(const std::string& project, const fs::path& bo2, const bool directMap)
    {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        const auto boot = fs::path(local ? local : L"") / "Plutonium" / "bin" / "plutonium-bootstrapper-win32.exe";
        std::vector<std::string> command{Str(boot), "t6zm", Str(bo2), "+set", "fs_game", "mods/" + project};
        if (directMap)
            command.insert(command.end(), {"-lan", "+devmap", project});
        return command;
    }
} // namespace w2b::pipeline
