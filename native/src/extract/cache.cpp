// Extraction caches (see cache.h).
#include "cache.h"
#include "extract/unlinker.h"

#include "core/asset_paths.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"
#include "core/profile.h"
#include "core/regex.h"
#include "core/share_files.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <thread>

namespace w2b::extract
{
    const char* const WAW_ASSETS = "clipmap,mapents,gfxworld,gameworldsp,material,techniqueset,image,fx,xmodel,weapon,xanim,sound,"
                                   "loadedsound,rawfile,comworld,lightdef,physpreset,physconstraints,snddriverglobals,localize,stringtable,font";

    namespace
    {
        std::mutex printMutex;

        void Print(const std::string& line)
        {
            std::lock_guard lock(printMutex);
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }

        std::string Hex(const size_t n)
        {
            static std::mutex m;
            static std::mt19937_64 random{std::random_device{}()};
            std::lock_guard lock(m);
            std::string out;
            while (out.size() < n)
            {
                char part[17];
                std::snprintf(part, sizeof part, "%016llx", static_cast<unsigned long long>(random()));
                out += part;
            }
            return out.substr(0, n);
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        std::string Posix(const fs::path& p)
        {
            return files::Utf8(p.generic_wstring());
        }

        void SortPaths(std::vector<fs::path>& paths)
        {
            const auto key = [](const fs::path& p)
            {
                std::vector<std::wstring> parts;
                for (const auto& part : p)
                {
                    auto text = part.wstring();
                    for (auto& c : text)
                        if (c >= L'A' && c <= L'Z')
                            c = static_cast<wchar_t>(c - L'A' + L'a');
                    parts.push_back(text);
                }
                return parts;
            };
            std::stable_sort(paths.begin(), paths.end(), [&](const fs::path& a, const fs::path& b) { return key(a) < key(b); });
        }

        void LinkOrCopy(const fs::path& source, const fs::path& target)
        {
            std::error_code e;
            fs::create_directories(target.parent_path(), e);
            fs::create_hard_link(source, target, e);
            if (e)
            {
                fs::copy_file(source, target, fs::copy_options::overwrite_existing);
                fs::last_write_time(target, fs::last_write_time(source, e), e);
            }
        }

        // folders under root whose extraction this process saw complete
        std::mutex verifiedMutex;
        std::set<std::tuple<std::wstring, uintmax_t, long long>> verified;

        bool Complete(const fs::path& folder, const fs::path& receipt, const util::Json& names)
        {
            std::error_code e;
            const auto identity = std::make_tuple(Resolve(receipt).wstring(), fs::file_size(receipt, e), Stamp(receipt)[1].get<long long>());
            {
                std::lock_guard lock(verifiedMutex);
                if (verified.count(identity))
                    return true;
            }
            for (const auto& name : names)
                if (!IsFile(folder / files::FromUtf8(name.get<std::string>())))
                    return false;
            std::lock_guard lock(verifiedMutex);
            verified.insert(identity);
            return true;
        }

        // the native unlinker is not long-path aware: extract near the cache top
        fs::path StagingFolder(const fs::path& root)
        {
            fs::path top = root;
            for (auto p = root; !p.empty(); p = p.parent_path())
            {
                if (files::Utf8(p.parent_path().filename()) == "asset_cache")
                {
                    top = p;
                    break;
                }
                if (p == p.root_path())
                    break;
            }
            return top / "x" / Hex(12);
        }

        util::Json WithoutKeys(const util::Json& object, std::initializer_list<const char*> drop)
        {
            util::Json out = util::Json::object();
            for (const auto& [k, v] : object.items())
                if (std::find_if(drop.begin(), drop.end(), [&](const char* d) { return k == d; }) == drop.end())
                    out[k] = v;
            return util::SortKeys(out);
        }

        std::set<std::string> SplitSet(const std::string& text)
        {
            std::set<std::string> out;
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); i++)
                if (i == text.size() || text[i] == ',')
                {
                    out.insert(text.substr(start, i - start));
                    start = i + 1;
                }
            return out;
        }

        std::optional<util::Json> ReuseCompactSubset(const fs::path& root, const fs::path& temporary, const util::Json& inputs)
        {
            if (inputs["assets"].is_null())
                return std::nullopt;
            const auto wanted = SplitSet(inputs["assets"].get<std::string>());
            const std::set<std::string> supported{"rawfile", "material", "techniqueset", "image", "snddriverglobals", "zbarrier", "xmodel"};
            for (const auto& w : wanted)
                if (!supported.count(w))
                    return std::nullopt;
            const auto comparable = WithoutKeys(inputs, {"schema", "assets"});
            std::set<std::string> prefixes{"zone_source"};
            const std::pair<const char*, std::vector<const char*>> kinds[] = {{"material", {"materials", "waw_techniquesets", "shader_bin"}},
                                                                              {"techniqueset", {"techniquesets", "waw_techniquesets", "shader_bin"}},
                                                                              {"image", {"images"}},
                                                                              {"snddriverglobals", {"sounddriverglobals"}},
                                                                              {"zbarrier", {"zbarrier"}},
                                                                              {"xmodel", {"xmodel", "model_export"}}};
            for (const auto& [kind, paths] : kinds)
                if (wanted.count(kind))
                    prefixes.insert(paths.begin(), paths.end());
            std::vector<fs::path> receipts;
            std::error_code e;
            for (fs::directory_iterator it(root, e), end; !e && it != end; it.increment(e))
                if (IsFile(it->path() / "extraction.json"))
                    receipts.push_back(it->path() / "extraction.json");
            SortPaths(receipts);
            for (const auto& receipt : receipts)
            {
                try
                {
                    const auto cached = ReadJson(receipt);
                    const auto& old = cached.at("inputs");
                    if (WithoutKeys(old, {"schema", "assets"}) != comparable)
                        continue;
                    if (!old.at("assets").is_null())
                    {
                        const auto had = SplitSet(old["assets"].get<std::string>());
                        if (!std::includes(had.begin(), had.end(), wanted.begin(), wanted.end()))
                            continue;
                    }
                    std::set<std::string> rawfiles;
                    if (cached.at("index").contains("rawfile"))
                        for (const auto& n : cached["index"]["rawfile"])
                        {
                            auto name = n.get<std::string>();
                            std::replace(name.begin(), name.end(), '\\', '/');
                            for (auto& c : name)
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            rawfiles.insert(name);
                        }
                    std::vector<std::string> keep;
                    for (const auto& n : cached.at("files"))
                    {
                        const auto name = n.get<std::string>();
                        auto lower = name;
                        for (auto& c : lower)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (prefixes.count(name.substr(0, name.find('/'))) || (wanted.count("rawfile") && rawfiles.count(lower)))
                            keep.push_back(name);
                    }
                    bool all = !keep.empty();
                    for (const auto& name : keep)
                        all = all && IsFile(receipt.parent_path() / files::FromUtf8(name));
                    if (!all)
                        continue;
                    for (const auto& name : keep)
                        LinkOrCopy(receipt.parent_path() / files::FromUtf8(name), temporary / files::FromUtf8(name));
                    Print("Extract All: retaining compact subset of " + files::Utf8(files::FromUtf8(inputs["file"].get<std::string>()).filename()) + " ("
                          + std::to_string(keep.size()) + "/" + std::to_string(cached["files"].size()) + " files)");
                    auto result = cached;
                    result["inputs"] = inputs;
                    result["files"] = keep;
                    return result;
                }
                catch (const std::exception&)
                {
                    continue;
                }
            }
            return std::nullopt;
        }

        void ShareDependencies(const fs::path& root, const fs::path& staging, const util::Json& inputs, const std::vector<std::string>& names)
        {
            if (files::Utf8(root.parent_path().filename()) != "dependencies")
                return;
            const auto identity = WithoutKeys(inputs, {"assets"});
            const auto wanted = SplitSet(inputs["assets"].is_null() ? "" : inputs["assets"].get<std::string>());
            std::vector<std::tuple<size_t, long long, fs::path>> candidates;
            std::error_code e;
            for (fs::directory_iterator it(root, e), end; !e && it != end; it.increment(e))
            {
                const auto receipt = it->path() / "extraction.json";
                try
                {
                    const auto old = ReadJson(receipt).at("inputs");
                    if (WithoutKeys(old, {"assets"}) != identity)
                        continue;
                    const auto had = SplitSet(old["assets"].is_null() ? "" : old["assets"].get<std::string>());
                    size_t common = 0;
                    for (const auto& h : had)
                        common += wanted.count(h);
                    candidates.emplace_back(common, Stamp(receipt)[1].get<long long>(), it->path());
                }
                catch (const std::exception&)
                {
                }
            }
            if (candidates.empty())
                return;
            std::sort(candidates.rbegin(), candidates.rend());
            std::vector<fs::path> prior, relative;
            for (size_t i = 0; i < std::min<size_t>(2, candidates.size()); i++)
                prior.push_back(std::get<2>(candidates[i]));
            for (const auto& name : names)
                relative.push_back(files::FromUtf8(name));
            const auto shared = files::ShareIdenticalFiles(staging, prior, relative);
            if (shared.files)
            {
                char text[64];
                std::snprintf(text, sizeof text, "%.1f", static_cast<double>(shared.bytes) / (1024.0 * 1024.0));
                Print("Extract All: shared " + std::to_string(shared.files) + " identical dependency files (" + text + " MiB)");
            }
        }
    } // namespace

    fs::path ResourceRoot()
    {
        if (const wchar_t* explicitRoot = _wgetenv(L"W2B_RESOURCE_ROOT"))
            return explicitRoot;
        wchar_t buffer[MAX_PATH * 4];
        const auto length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        const fs::path folder = fs::path(std::wstring(buffer, length)).parent_path();
        std::error_code e;
        for (auto probe = folder; !probe.empty(); probe = probe.parent_path())
        {
            if (fs::is_directory(probe / "vendor", e) || fs::is_directory(probe / "tools" / "bin", e))
                return probe;
            if (probe == probe.root_path())
                break;
        }
        return folder;
    }

    std::string Str(fs::path path)
    {
        return files::Utf8(path.make_preferred());
    }

    fs::path Resolve(const fs::path& path)
    {
        std::error_code e;
        const auto absolute = fs::absolute(path, e);
        const auto resolved = fs::weakly_canonical(absolute, e);
        return e ? absolute.lexically_normal() : resolved;
    }

    std::string Key(const util::Json& data)
    {
        return hash::Sha256Hex(util::JsonDumpsSorted(data)).substr(0, 24);
    }

    std::string Digest(const fs::path& path)
    {
        std::string bytes;
        if (!files::ReadBytes(path, bytes))
            throw Error("FileNotFoundError", "[Errno 2] No such file or directory: '" + Str(path) + "'");
        return hash::Sha256Hex(bytes);
    }

    util::Json Stamp(const fs::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            throw Error("FileNotFoundError", "[WinError 2] The system cannot find the file specified: '" + Str(path) + "'");
        ULARGE_INTEGER t;
        t.LowPart = data.ftLastWriteTime.dwLowDateTime;
        t.HighPart = data.ftLastWriteTime.dwHighDateTime;
        const long long size = static_cast<long long>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
        return util::Json::array({size, (static_cast<long long>(t.QuadPart) - 116444736000000000LL) * 100});
    }

    std::string SearchPaths(const std::vector<fs::path>& paths)
    {
        std::string out;
        std::error_code e;
        for (const auto& p : paths)
            if (fs::is_directory(p, e))
                out += (out.empty() ? "" : ";") + Str(p);
        return out;
    }

    void Publish(const fs::path& source, const fs::path& target, const bool overwrite)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        auto delay = std::chrono::milliseconds(50);
        while (true)
        {
            if (MoveFileExW(source.c_str(), target.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0))
                return;
            const auto error = GetLastError();
            if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) || std::chrono::steady_clock::now() >= deadline)
                throw Error("PermissionError", "could not move " + Str(source) + " to " + Str(target)
                                                   + "; another process (antivirus, indexer, editor) may be holding it (error " + std::to_string(error) + ")");
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(500));
        }
    }

    void WriteJson(const fs::path& path, const util::Json& data)
    {
        const auto temporary = path.parent_path() / files::FromUtf8(files::Utf8(path.filename()) + ".tmp");
        files::WriteBytes(temporary, util::JsonDumpsSorted(data));
        Publish(temporary, path, true);
    }

    util::Json ReadJson(const fs::path& path)
    {
        return util::Json::parse(files::ReadText(path));
    }

    CacheLock::CacheLock(const fs::path& root)
    {
        fs::create_directories(root);
        const HANDLE file = CreateFileW((root / "extract.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw Error("RuntimeError", "Game assets are being extracted by another process: " + Str(root));
        DWORD written = 0;
        SetFilePointer(file, 0, nullptr, FILE_END);
        WriteFile(file, "0", 1, &written, nullptr);
        OVERLAPPED at{};
        if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at))
        {
            CloseHandle(file);
            throw Error("RuntimeError", "Game assets are being extracted by another process: " + Str(root));
        }
        handle = file;
    }

    CacheLock::~CacheLock()
    {
        if (handle)
            CloseHandle(handle);
    }

    void RunTool(const std::vector<std::string>& command, const fs::path& log)
    {
        fs::create_directories(log.parent_path());
        const char* flag = std::getenv("WAW2BO2_VERBOSE");
        const bool verbose = flag && std::string(flag) == "1";
        const auto once = [&]
        {
            Print("[tool] " + files::Utf8(files::FromUtf8(command[0]).filename()) + "; log: " + Str(log));
            const auto result = RunUnlinker(command, verbose ? +[](const std::string& line) { Print(line); } : nullptr);
            files::WriteBytes(log, result.out);
            return result;
        };
        auto result = once();
        for (int attempt = 0; attempt < 2; attempt++)
        {
            const auto code = static_cast<unsigned>(result.exitCode);
            if (code != 0xC0000005u && code != 0xC0000409u)
                break;
            char hex[16];
            std::snprintf(hex, sizeof hex, "%08X", code);
            Print("Extract All: " + files::Utf8(files::FromUtf8(command.back()).filename()) + ": unlinker crashed (0x" + hex + "); retrying (" + std::to_string(attempt + 1)
                  + "/2)");
            result = once();
        }
        if (result.exitCode)
        {
            const auto text = encoding::Utf8Replace(result.out);
            Print(text.substr(text.size() > 8000 ? text.size() - 8000 : 0));
            throw Error("RuntimeError", "Extraction failed (" + std::to_string(static_cast<unsigned>(result.exitCode)) + "); see " + Str(log));
        }
    }

    std::map<std::string, std::vector<std::string>> Listing(const std::string& text, const bool references)
    {
        std::map<std::string, std::vector<std::string>> assets;
        const auto listing = ListingJson(text, references);
        for (const auto& kv : listing.items())
            assets[kv.key()] = kv.value().get<std::vector<std::string>>();
        return assets;
    }

    util::Json ListingJson(const std::string& text, const bool references)
    {
        const auto strip = [](std::string_view s) { return std::string(util::Strip(s)); };
        util::Json assets = util::Json::object();
        for (const auto line : util::SplitLines(text))
        {
            const auto comma = line.find(',');
            if (comma == std::string_view::npos)
                continue;
            const auto kind = strip(line.substr(0, comma));
            const auto name = strip(line.substr(comma + 1));
            std::string bare;
            for (const char c : kind)
                if (c != '_')
                    bare += c;
            const bool alnum = !bare.empty() && std::all_of(bare.begin(), bare.end(), [](const unsigned char c) { return c >= 0x80 || std::isalnum(c); });
            if (name.empty() || (!references && name.front() == ',') || !alnum)
                continue;
            if (!assets.contains(kind))
                assets[kind] = util::Json::array();
            assets[kind].push_back(name.substr(name.find_first_not_of(',') == std::string::npos ? name.size() : name.find_first_not_of(',')));
        }
        return assets;
    }

    namespace
    {
        // an earlier generation of the same zone that the extraction extends:
        // its receipt, and the asset kinds still to extract
        struct BaseGeneration
        {
            fs::path folder;
            util::Json receipt;
            std::string missingKinds;
        };

        // Kinds dump independently: a dump of A+B is byte for byte the dump of A
        // merged with the dump of B (measured on sound, model, material, image,
        // technique set, font and light def zones), so a zone extracted again
        // for more kinds extracts only the new ones.
        std::optional<BaseGeneration> ExtendableBase(const ZoneRequest& r, const util::Json& inputs)
        {
            if (!r.base || !r.assets)
                return std::nullopt;
            try
            {
                auto receipt = ReadJson(*r.base / "extraction.json");
                const auto& old = receipt.at("inputs");
                if (old.at("assets").is_null() || WithoutKeys(old, {"schema", "assets"}) != WithoutKeys(inputs, {"schema", "assets"}) ||
                    !Complete(*r.base, *r.base / "extraction.json", receipt.at("files")))
                    return std::nullopt;
                const auto had = SplitSet(old["assets"].get<std::string>());
                const auto wanted = SplitSet(*r.assets);
                if (!std::includes(wanted.begin(), wanted.end(), had.begin(), had.end()))
                    return std::nullopt;
                std::string missing;
                for (const auto& kind : wanted)
                    if (!had.count(kind))
                        missing += (missing.empty() ? "" : ",") + kind;
                if (missing.empty())
                    return std::nullopt;
                return BaseGeneration{*r.base, std::move(receipt), std::move(missing)};
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        // the earlier generation's files the new kinds did not write, hard
        // linked (generations are immutable) or copied across volumes
        void AdoptBaseFiles(const BaseGeneration& base, const fs::path& temporary)
        {
            for (const auto& entry : base.receipt.at("files"))
            {
                const auto relative = files::FromUtf8(entry.get<std::string>());
                const auto target = temporary / relative;
                std::error_code e;
                if (fs::exists(target, e))
                    continue;
                fs::create_directories(target.parent_path(), e);
                fs::create_hard_link(base.folder / relative, target, e);
                if (e)
                    fs::copy_file(base.folder / relative, target, fs::copy_options::overwrite_existing);
            }
        }
    } // namespace

    Extraction ExtractZone(const ZoneRequest& r)
    {
        util::Json inputs = util::Json::object();
        inputs["schema"] = SCHEMA;
        inputs["file"] = Str(Resolve(r.ff));
        inputs["stamp"] = Stamp(r.ff);
        inputs["tool"] = r.toolDigest ? *r.toolDigest : UnlinkerIdentity(r.tool);
        inputs["dependencies"] = r.dependencies;
        inputs["assets"] = r.assets ? util::Json(*r.assets) : util::Json();
        inputs["images"] = r.imageFormat;
        inputs["models"] = "GLTF";
        inputs["list"] = r.list;
        inputs = util::SortKeys(inputs);
        const auto key = Key(inputs);
        auto folder = r.root / files::FromUtf8(key);
        std::error_code e;
        if (!r.refresh)
        {
            try
            {
                const auto pointer = ReadJson(r.root / files::FromUtf8(key + ".json")).at("folder").get<std::string>();
                const auto candidate = r.root / files::FromUtf8(pointer);
                if (Resolve(candidate.parent_path()) == Resolve(r.root))
                    folder = candidate;
            }
            catch (const std::exception&)
            {
            }
        }
        const auto receipt = folder / "extraction.json";
        try
        {
            const profile::Scope scope("extract: reuse a cached zone");
            const auto cached = ReadJson(receipt);
            if (!r.refresh && util::SortKeys(cached.at("inputs")) == inputs && Complete(folder, receipt, cached.at("files")))
                return {folder, cached};
        }
        catch (const std::exception&)
        {
        }
        if (fs::exists(folder, e))
            folder = r.root / files::FromUtf8(key + "-" + Hex(8));
        const auto temporary = StagingFolder(r.root);
        fs::create_directories(temporary);
        if (!r.refresh)
            if (auto migrated = ReuseCompactSubset(r.root, temporary, inputs))
            {
                WriteJson(temporary / "extraction.json", *migrated);
                Publish(temporary, folder);
                WriteJson(r.root / files::FromUtf8(key + ".json"), util::Json{{"folder", files::Utf8(folder.filename())}});
                return {folder, *migrated};
            }
        const auto base = r.refresh ? std::nullopt : ExtendableBase(r, inputs);
        const auto log = r.root / files::FromUtf8(files::Utf8(r.ff.stem()) + ".log");
        std::vector<std::string> command{Str(r.tool), "--no-color", "--search-path", r.search, "--image-format", r.imageFormat, "--model-format", "GLTF",
                                         "--output-folder", Str(temporary)};
        if (base)
            command.insert(command.end(), {"--include-assets", base->missingKinds});
        else if (r.assets)
            command.insert(command.end(), {"--include-assets", *r.assets});
        command.push_back(Str(r.ff));
        util::Json index = util::Json::object(), loadedIndex = util::Json::object();
        try
        {
            RunTool(command, log);
            static const regex::Regex failed(R"((?m)^ERROR: Failed to open file '([^']+)')");
            std::string bytes;
            files::ReadBytes(log, bytes);
            std::vector<std::string> unwritten, required;
            for (const auto& row : failed.FindAll(encoding::Utf8Replace(bytes)))
                if (!IsFile(temporary / files::FromUtf8(row[0])))
                {
                    unwritten.push_back(row[0]);
                    if (row[0].starts_with("model_export/") || row[0].starts_with("images/") || row[0].starts_with("xmodel/"))
                        required.push_back(row[0]);
                }
            if (!required.empty())
                throw Error("RuntimeError", files::Utf8(r.ff.filename()) + ": the unlinker could not write " + std::to_string(required.size()) + " model/image file(s), e.g. "
                                                + required[0] + " (path too long?); see " + Str(log));
            if (!unwritten.empty())
                Print("Extract All: warning: " + files::Utf8(r.ff.filename()) + ": " + std::to_string(unwritten.size()) + " file(s) could not be written, e.g. " + unwritten[0]
                      + "; see " + Str(log));
            if (base)
                AdoptBaseFiles(*base, temporary);
            if (r.list && base && base->receipt.at("inputs").value("list", false))
            {
                // the zone's listing does not depend on the kinds extracted
                index = base->receipt.at("index");
                loadedIndex = base->receipt.at("loaded_index");
            }
            else if (r.list)
            {
                const auto listLog = r.root / files::FromUtf8(files::Utf8(r.ff.stem()) + ".assets.log");
                RunTool({Str(r.tool), "--no-color", "--list", "--search-path", r.search, Str(r.ff)}, listLog);
                std::string listed;
                files::ReadBytes(listLog, listed);
                const auto text = encoding::UniversalNewlines(encoding::Utf8Replace(listed));
                index = ListingJson(text);
                loadedIndex = ListingJson(text, true);
            }
        }
        catch (...)
        {
            fs::remove_all(temporary, e);
            throw;
        }
        std::vector<std::string> names;
        for (fs::recursive_directory_iterator it(temporary, e), end; !e && it != end; it.increment(e))
            if (it->is_regular_file(e))
                names.push_back(Posix(it->path().lexically_relative(temporary)));
        std::sort(names.begin(), names.end());
        if (names.empty())
            throw Error("RuntimeError", "Extractor returned success without writing files for " + Str(r.ff) + "; see " + Str(log));
        ShareDependencies(r.root, temporary, inputs, names);
        util::Json data = util::Json::object();
        data["inputs"] = inputs;
        data["files"] = names;
        data["index"] = index;
        data["loaded_index"] = loadedIndex;
        WriteJson(temporary / "extraction.json", data);
        Publish(temporary, folder);
        WriteJson(r.root / files::FromUtf8(key + ".json"), util::Json{{"folder", files::Utf8(folder.filename())}});
        return {folder, data};
    }

    std::vector<util::Json> ParallelZones(const std::vector<fs::path>& zones, const std::function<util::Json(const fs::path&)>& extract, const std::string& label,
                                        std::optional<int> workers)
    {
        if (zones.empty())
            return {};
        if (!workers)
        {
            const char* a = std::getenv("WAW2BO2_EXTRACT_WORKERS");
            const char* b = std::getenv("WAW2BO2_WORKERS");
            workers = a && *a ? std::atoi(a) : b && *b ? std::atoi(b) : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        }
        if (*workers < 1)
            throw Error("ValueError", "Extraction worker count must be at least 1");
        const int count = std::min<int>(*workers, static_cast<int>(zones.size()));
        Print("Extract All: " + label + ": " + std::to_string(count) + " parallel workers, " + std::to_string(zones.size()) + " zones");
        const auto started = std::chrono::steady_clock::now();
        std::vector<util::Json> results(zones.size());
        std::atomic<size_t> next{0}, completed{0};
        std::atomic<bool> stop{false};
        std::exception_ptr failure;
        std::mutex failureMutex;
        std::vector<std::thread> pool;
        for (int w = 0; w < count; w++)
            pool.emplace_back(
                [&]
                {
                    while (!stop)
                    {
                        const size_t i = next++;
                        if (i >= zones.size())
                            return;
                        try
                        {
                            results[i] = extract(zones[i]);
                            char seconds[32];
                            std::snprintf(seconds, sizeof seconds, "%.1f", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
                            Print("Extract All: " + label + ": " + std::to_string(++completed) + "/" + std::to_string(zones.size()) + " zones completed (" + seconds + "s)");
                        }
                        catch (...)
                        {
                            // finish active jobs, start no queued work
                            stop = true;
                            std::lock_guard lock(failureMutex);
                            if (!failure)
                                failure = std::current_exception();
                        }
                    }
                });
        for (auto& t : pool)
            t.join();
        if (failure)
            std::rethrow_exception(failure);
        return results;
    }

    std::optional<fs::path> RequiredBo2Image(const fs::path& stock, const std::string& name, const std::optional<fs::path>& tool)
    {
        const auto catalogFile = stock / "catalog.json";
        if (!IsFile(catalogFile))
            return std::nullopt;
        const auto catalog = ReadJson(catalogFile);
        for (const auto& [_, entry] : catalog["zones"].items())
        {
            const auto images = entry["index"].value("image", util::Json::array());
            if (std::find(images.begin(), images.end(), util::Json(name)) == images.end())
                continue;
            const auto ff = files::FromUtf8(entry["inputs"]["file"].get<std::string>());
            const auto unlinker = tool.value_or(ResourceRoot() / "vendor/OpenAssetToolsT6/build/bin/Release_x86/Unlinker.exe");
            const auto root = stock.parent_path().parent_path() / "dependencies/images" / files::FromUtf8(Key(util::Json::array({Str(ff), name})));
            const auto relative = assets::ImagePath(name, ".iwi");
            CacheLock lock(root);
            ZoneRequest request{ff, unlinker, root, SearchPaths({ff.parent_path(), ff.parent_path().parent_path().parent_path()}), std::string("image"), "IWI"};
            request.list = false;
            auto [output, data] = ExtractZone(request);
            const auto wanted = output / relative;
            if (!IsFile(wanted))
                return std::nullopt;
            const auto keep = Posix(relative);
            for (const auto& filename : data["files"])
                if (filename.get<std::string>() != keep)
                    fs::remove(output / files::FromUtf8(filename.get<std::string>()));
            data["files"] = util::Json::array({keep});
            WriteJson(output / "extraction.json", data);
            return wanted;
        }
        return std::nullopt;
    }

    std::optional<fs::path> RequiredBo2Techset(const fs::path& stock, const std::string& name, const std::optional<fs::path>& tool)
    {
        const auto catalogFile = stock / "catalog.json";
        if (!IsFile(catalogFile))
            return std::nullopt;
        const auto catalog = ReadJson(catalogFile);
        for (const auto& [_, entry] : catalog["zones"].items())
        {
            const auto sets = entry["index"].value("techniqueset", util::Json::array());
            if (std::find(sets.begin(), sets.end(), util::Json(name)) == sets.end())
                continue;
            const auto ff = files::FromUtf8(entry["inputs"]["file"].get<std::string>());
            const auto unlinker = tool.value_or(ResourceRoot() / "vendor/OpenAssetToolsT6/build/bin/Release_x86/Unlinker.exe");
            const auto root = stock.parent_path().parent_path() / "dependencies/techniquesets" / files::FromUtf8(Key(util::Json::array({Str(ff)})));
            fs::path output;
            {
                CacheLock lock(root);
                ZoneRequest request{ff, unlinker, root, SearchPaths({ff.parent_path(), ff.parent_path().parent_path().parent_path()}), std::string("techniqueset"), "IWI"};
                request.list = false;
                output = ExtractZone(request).folder;
            }
            if (IsFile(output / "techniquesets" / files::FromUtf8(name + ".json")))
                return output;
        }
        return std::nullopt;
    }
} // namespace w2b::extract
