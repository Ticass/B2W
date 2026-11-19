// WaW assets compiled from the WaW Mod Tools sources (see wawsource.h).
#include "extract/wawsource.h"
#include "extract/unlinker.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/process.h"
#include "extract/cache.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace w2b::extract
{
    namespace
    {
        const fs::path LINKER = fs::path("bin") / "linker_pc.exe";
        constexpr const char* DUMP_ASSETS = "material,image,fx,xmodel";
        // v3: verify the requested asset was actually dumped
        constexpr const char* DUMP_MARKER = ".waw2bo2_source_dump_v3";
        // after an unrecoverable error the linker waits for a key press
        constexpr double LINK_TIMEOUT = 300;
        constexpr std::size_t ZONE_NAME_LIMIT = 63;

        bool IsFile(const fs::path& path)
        {
            std::error_code e;
            return fs::is_regular_file(path, e);
        }

        bool IsDir(const fs::path& path)
        {
            std::error_code e;
            return fs::is_directory(path, e);
        }

        bool IsUnder(const fs::path& path, const fs::path& root)
        {
            const auto relative = path.lexically_relative(root);
            return !relative.empty() && *relative.begin() != "..";
        }

        // a directory junction link -> target (_winapi.CreateJunction)
        void CreateJunction(const fs::path& link, const fs::path& target)
        {
            std::error_code e;
            if (fs::exists(link, e))
                return;
            if (!CreateDirectoryW(link.c_str(), nullptr))
                throw WawSourceError("cannot create junction folder " + Str(link));
            const auto absolute = fs::absolute(target).wstring();
            const std::wstring substitute = L"\\??\\" + absolute;
            const auto substituteBytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
            const auto printBytes = static_cast<USHORT>(absolute.size() * sizeof(wchar_t));
            // REPARSE_DATA_BUFFER for a mount point: header, then both names NUL-terminated
            const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
            const std::size_t total = 8 + 8 + pathBytes;
            std::vector<unsigned char> buffer(total);
            const auto put16 = [&](std::size_t offset, USHORT value) { std::memcpy(buffer.data() + offset, &value, 2); };
            const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
            std::memcpy(buffer.data(), &tag, 4);
            put16(4, static_cast<USHORT>(total - 8));
            put16(8, 0);                                                     // SubstituteNameOffset
            put16(10, substituteBytes);                                      // SubstituteNameLength
            put16(12, static_cast<USHORT>(substituteBytes + sizeof(wchar_t))); // PrintNameOffset
            put16(14, printBytes);                                           // PrintNameLength
            std::memcpy(buffer.data() + 16, substitute.c_str(), substituteBytes);
            std::memcpy(buffer.data() + 16 + substituteBytes + sizeof(wchar_t), absolute.c_str(), printBytes);
            const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            DWORD returned = 0;
            const bool ok = handle != INVALID_HANDLE_VALUE &&
                            DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(total), nullptr, 0, &returned, nullptr);
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            if (!ok)
            {
                RemoveDirectoryW(link.c_str());
                throw WawSourceError("cannot create junction " + Str(link) + " -> " + Str(target));
            }
        }

        // where the unlinker writes a compiled fx, material or xmodel (assetresolve.asset_path)
        fs::path DumpedPath(const std::string& kind, const std::string& name)
        {
            if (kind == "material")
                return assets::MaterialPath(name);
            return fs::path(kind) / files::FromUtf8(name + (kind == "fx" ? ".w2bfx.json" : ".json"));
        }

        std::string Lower(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    } // namespace

    std::string SourceZoneName(const std::string& kind, const std::string& name)
    {
        std::string zone = kind + "_" + name;
        for (auto& c : zone)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                c = '_';
        zone = "w2bsrc_" + Lower(zone);
        if (zone.size() <= ZONE_NAME_LIMIT)
            return zone;
        return zone.substr(0, ZONE_NAME_LIMIT - 11) + "_" + hash::Sha1Hex(zone).substr(0, 10);
    }

    WawSource::WawSource(fs::path modTools, fs::path wawRoot, fs::path unlinker, fs::path work)
        : modTools_(std::move(modTools)), wawRoot_(std::move(wawRoot)), unlinker_(std::move(unlinker)), work_(std::move(work))
    {
    }

    std::optional<fs::path> WawSource::Locate(const std::optional<fs::path>& explicitFolder, const std::optional<fs::path>& wawRoot,
                                              const fs::path& resourceRoot)
    {
        if (explicitFolder)
            return explicitFolder;
        for (const auto& candidate : {wawRoot, std::optional(resourceRoot / "wawModTools")})
            if (candidate && fs::exists(*candidate / LINKER))
                return candidate;
        return std::nullopt;
    }

    fs::path WawSource::Raw() const
    {
        // the installer puts raw/ into the WaW install; a self-contained Mod
        // Tools folder may carry its own
        for (const auto& candidate : {modTools_ / "raw", wawRoot_ / "raw"})
            if (IsDir(candidate / "fx"))
                return candidate;
        return wawRoot_ / "raw";
    }

    std::vector<std::string> WawSource::Check() const
    {
        std::vector<std::string> problems;
        if (!fs::exists(modTools_ / LINKER))
            problems.push_back("WaW linker " + Str(modTools_ / LINKER) + " missing");
        if (!IsDir(Raw() / "fx"))
            problems.push_back("no raw/fx in " + Str(modTools_) + " or " + Str(wawRoot_));
        if (!IsDir(wawRoot_ / "main"))
            problems.push_back("no main/ in " + Str(wawRoot_));
        return problems;
    }

    std::optional<fs::path> WawSource::Source(const std::string& kind, const std::string& name) const
    {
        std::vector<fs::path> raws{modTools_ / "raw"};
        if (wawRoot_ / "raw" != raws.front())
            raws.push_back(wawRoot_ / "raw");
        for (const auto& raw : raws)
        {
            fs::path candidate;
            if (kind == "xmodel")
                candidate = raw / "xmodel" / files::FromUtf8(name);
            else if (kind == "fx")
                candidate = raw / "fx" / files::FromUtf8(name + ".efx");
            else if (kind == "material")
            {
                candidate = raw / "materials" / files::FromUtf8(name);
                if (IsFile(candidate))
                    return candidate;
                // draw-family variants (mc/, wc/, mlv/) compile from the plain source
                const auto slash = name.find('/');
                const auto family = name.substr(0, slash);
                if (slash != std::string::npos && (family == "mc" || family == "wc" || family == "mlv"))
                    candidate = raw / "materials" / files::FromUtf8(name.substr(slash + 1));
            }
            else
                continue;
            if (IsFile(candidate))
                return candidate;
        }
        return std::nullopt;
    }

    fs::path WawSource::Workspace(const fs::path& raw) const
    {
        // the linker resolves ../raw, ../zone_source, ../zone and the game files
        // (main/iw_00.iwd holds fileSysCheck.cfg) relative to its working folder
        const auto workspace = work_ / "linker" / files::FromUtf8(Key(py::Json(Str(Resolve(raw)))));
        for (const char* folder : {"bin", "zone_source", "zone/english"})
            fs::create_directories(workspace / folder);
        CreateJunction(workspace / "raw", raw);
        CreateJunction(workspace / "main", wawRoot_ / "main");
        return workspace;
    }

    std::optional<fs::path> WawSource::Compile(const std::string& kind, const std::string& name)
    {
        const auto source = Source(kind, name);
        if (!source)
            return std::nullopt;
        const auto raw = IsUnder(*source, modTools_ / "raw") ? modTools_ / "raw" : wawRoot_ / "raw";
        // engine $ materials compile without a draw-family variant; models
        // imported from other CoD tools can nevertheless name mc/$default3d
        const auto slash = name.find('/');
        const auto family = name.substr(0, slash == std::string::npos ? 0 : slash);
        const auto base = slash == std::string::npos ? name : name.substr(slash + 1);
        if (kind == "material" && (family == "mc" || family == "wc" || family == "mlv") && base.starts_with('$') &&
            *source == raw / "materials" / files::FromUtf8(base))
        {
            const auto recovered = Compile(kind, base);
            const auto alias = *recovered / assets::MaterialPath(name);
            fs::create_directories(alias.parent_path());
            fs::copy_file(*recovered / assets::MaterialPath(base), alias, fs::copy_options::overwrite_existing);
            return recovered;
        }
        const auto zone = SourceZoneName(kind, name);
        const auto out = work_ / files::FromUtf8(zone);
        const auto stamp = Stamp(*source);
        const auto marker = out / DUMP_MARKER;
        const auto markerText = Str(Resolve(*source)) + " " + std::to_string(stamp[1].get<long long>()) + " " + std::to_string(stamp[0].get<long long>());
        std::string existing;
        if (files::ReadBytes(marker, existing) && existing == markerText)
            return out;
        const auto workspace = Workspace(raw);
        files::WriteText(workspace / "zone_source" / files::FromUtf8(zone + ".csv"), kind + "," + name + "\n");
        const auto fastfile = workspace / "zone/english" / files::FromUtf8(zone + ".ff");
        std::error_code e;
        fs::remove(fastfile, e);
        const auto log = work_ / files::FromUtf8(zone + ".link.log");
        const auto linked = process::RunMerged({Str(modTools_ / LINKER), "-language", "english", zone}, files::Utf8(workspace / "bin"), nullptr, LINK_TIMEOUT);
        files::WriteBytes(log, linked.out);
        const auto what = kind + " " + name + " (see " + Str(log) + ")";
        if (linked.timedOut)
            throw WawSourceError("WaW linker hung compiling " + what);
        if (!fs::exists(fastfile) || linked.out.find("UNRECOVERABLE ERROR") != std::string::npos)
            throw WawSourceError("WaW linker failed compiling " + what);
        if (fs::exists(out))
        {
            // a changed source must not inherit an asset from an older dump
            // when the linker emits an incomplete fastfile
            if (fs::is_symlink(out) || !IsUnder(Resolve(out), Resolve(work_)))
                throw WawSourceError("source dump is outside the work folder: " + Str(out));
            fs::remove_all(out);
        }
        const auto dumpLog = work_ / files::FromUtf8(zone + ".dump.log");
        const auto dumped = RunUnlinker({Str(unlinker_), "--no-color", "--search-path", Str(wawRoot_ / "main"), "--image-format", "DDS", "--model-format",
                                                "GLTF", "--include-assets", DUMP_ASSETS, "--output-folder", Str(out), Str(fastfile)});
        files::WriteBytes(dumpLog, dumped.out);
        if (dumped.exitCode)
            throw WawSourceError("dump of compiled source " + kind + " " + name + " failed (" + std::to_string(dumped.exitCode) + "); see " + Str(dumpLog));
        if (!IsFile(out / DumpedPath(kind, name)))
            throw WawSourceError("WaW linker produced no " + what);
        files::WriteBytes(marker, markerText);
        return out;
    }

    std::vector<std::string> WawSource::LinkWarnings(const std::string& kind, const std::string& name) const
    {
        std::string text;
        if (!files::ReadBytes(work_ / files::FromUtf8(SourceZoneName(kind, name) + ".link.log"), text))
            return {};
        std::vector<std::string> warnings;
        for (const auto line : py::SplitLines(text))
        {
            const auto stripped = std::string(py::Strip(line));
            if (line.find("failed loading") != std::string_view::npos || stripped.ends_with("not found"))
                warnings.push_back(stripped);
        }
        return warnings;
    }
} // namespace w2b::extract
