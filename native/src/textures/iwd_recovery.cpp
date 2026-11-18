#include "textures/iwd_recovery.h"

#include "core/files.h"
#include "core/pytext.h"
#include "textures/dds.h"
#include "textures/iwi6.h"

#include <algorithm>
#include <windows.h>

namespace w2b::textures
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string LowerName(std::string text)
        {
            if (std::ranges::all_of(text, [](unsigned char c) { return c < 128; }))
            {
                for (auto& c : text)
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c + ('a' - 'A'));
                return text;
            }
            const auto wide = files::FromUtf8(text).wstring();
            const auto count = static_cast<int>(wide.size());
            const auto needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
                                              count, nullptr, 0, nullptr, nullptr, 0);
            if (needed > 0)
            {
                std::wstring lowered(needed, L'\0');
                if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
                                  count, lowered.data(), needed, nullptr, nullptr, 0))
                    return files::Utf8(fs::path(lowered));
            }
            return text;
        }

        fs::path Resolved(const fs::path& path)
        {
            auto result = fs::weakly_canonical(fs::absolute(path));
            if (!result.has_filename() && result.has_relative_path())
                result = result.parent_path();
            return result;
        }

        bool SamePart(const fs::path& a, const fs::path& b)
        {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
        }

        bool IsRelativeTo(const fs::path& path, const fs::path& root)
        {
            auto current = path.begin();
            for (const auto& component : root)
            {
                if (current == path.end() || !SamePart(*current, component))
                    return false;
                ++current;
            }
            return true;
        }

        std::string ImageKey(const std::string& stem)
        {
            return "images/" + LowerName(stem) + ".iwi";
        }

        fs::path Destination(const fs::path& root, const std::string& stem, bool wavelet)
        {
            const auto relative = files::FromUtf8(stem + ".dds");
            const auto target = root / relative;
            if (relative.has_root_path() || std::ranges::find(relative, fs::path("..")) != relative.end() ||
                !IsRelativeTo(Resolved(target), Resolved(root)))
                throw TextureError(std::string(wavelet ? "unsafe wavelet image name " : "unsafe image name ") +
                                   py::StringRepr(stem));
            return target;
        }
    }

    IwdRecovery::IwdRecovery(std::span<const fs::path> directories, fs::path root, std::optional<fs::path> binary,
                             std::span<const fs::path> sourceImageRoots, std::span<const fs::path> overrides)
        : root_(std::move(root) / "images"), binary_(std::move(binary))
    {
        for (const auto& source : sourceImageRoots)
            sourceImageRoots_.push_back(Resolved(source));
        std::vector<fs::path> overridePaths;
        for (const auto& path : overrides)
            overridePaths.push_back(Resolved(path));
        for (const auto& directory : directories)
        {
            if (!fs::is_directory(directory))
                continue;
            const auto resolved = Resolved(directory);
            const bool hasOverridePriority = std::ranges::any_of(overridePaths, [&](const fs::path& path)
                { return SamePart(path, resolved); });
            std::vector<fs::path> archives;
            for (const auto& entry : fs::directory_iterator(directory))
                if (LowerName(files::Utf8(entry.path().filename())).ends_with(".iwd"))
                    archives.push_back(entry.path());
            std::ranges::stable_sort(archives, std::ranges::greater{}, [](const fs::path& path)
                { return LowerName(files::Utf8(path.filename())); });
            for (const auto& archive : archives)
                AddArchive(archive, hasOverridePriority);
        }
    }

    void IwdRecovery::AddArchive(const fs::path& path, bool overrides)
    {
        const auto index = archives_.size();
        auto archive = std::make_unique<zip::Archive>(path);
        const auto& members = archive->Entries();
        std::unordered_map<std::string_view, std::size_t> lastExactName;
        for (std::size_t i = 0; i < members.size(); ++i)
            lastExactName[members[i].name] = i;
        for (const auto& member : members)
        {
            auto key = LowerName(member.name);
            if (key.starts_with("images/") && key.ends_with(".iwi"))
                entries_[std::move(key)].push_back({index, lastExactName.at(member.name)});
        }
        archives_.push_back({path, overrides, std::move(archive)});
    }

    std::unordered_set<std::string> IwdRecovery::ImageStems(std::span<const fs::path> directories) const
    {
        std::unordered_set<std::string> names;
        const auto add = [&](const zip::Archive& archive)
        {
            for (const auto& entry : archive.Entries())
                if (auto name = LowerName(entry.name); name.ends_with(".iwi"))
                    names.insert(files::Utf8(files::FromUtf8(name).stem()));
        };
        for (const auto& dir : directories)
        {
            if (!fs::is_directory(dir))
                continue;
            for (const auto& entry : fs::directory_iterator(dir))
            {
                const auto& path = entry.path();
                if (!LowerName(files::Utf8(path.extension())).ends_with(".iwd"))
                    continue;
                const auto found = std::ranges::find_if(archives_, [&](const Archive& archive) { return SamePart(path, archive.path); });
                if (found == archives_.end())
                    add(zip::Archive(path));
                else
                    add(*found->contents);
            }
        }
        return names;
    }

    std::string IwdRecovery::Read(Source source)
    {
        auto& archive = *archives_[source.archive].contents;
        return archive.Read(archive.Entries()[source.entry]);
    }

    bool IwdRecovery::Shadowed(std::span<const Source> copies, const fs::path& source)
    {
        if (copies.size() < 2)
            return false;
        std::string data;
        if (!files::ReadBytes(source, data))
            return false;
        std::string_view pixels;
        try
        {
            pixels = ReadDds(data).payload;
        }
        catch (const TextureError&)
        {
            return false;
        }
        const auto samePixels = [&](Source copy)
        {
            const auto dds = CompressedIwi6ToDds(Read(copy));
            return dds && ReadDds(*dds).payload == pixels;
        };
        return !samePixels(copies.front()) && std::ranges::any_of(copies.subspan(1), samePixels);
    }

    RecoveredImage IwdRecovery::Resolve(const std::string& stem, const fs::path& source)
    {
        const auto found = entries_.find(ImageKey(stem));
        const std::span<const Source> copies = found == entries_.end() ? std::span<const Source>{} :
            std::span<const Source>(found->second);
        if (!source.empty())
        {
            const auto path = Resolved(source);
            if (std::ranges::any_of(sourceImageRoots_, [&](const fs::path& root) { return IsRelativeTo(path, root); }))
                return Shadowed(copies, source) ? Recover(stem) : RecoveredImage{source, {}, {}, 0, {}};
        }
        if (!copies.empty() && archives_[copies.front().archive].overrides)
        {
            auto recovered = Recover(stem);
            if (recovered.path.empty())
            {
                const auto& hit = copies.front();
                const auto& archive = archives_[hit.archive];
                throw TextureError("map IWD image " + stem + ": unsupported IWI format in " +
                    files::Utf8(archive.path) + ":" + archive.contents->Entries()[hit.entry].name);
            }
            return recovered;
        }
        return source.empty() ? Recover(stem) : RecoveredImage{source, {}, {}, 0, {}};
    }

    RecoveredImage IwdRecovery::Recover(const std::string& stem)
    {
        if (const auto cached = recovered_.find(stem); cached != recovered_.end())
            return cached->second;
        const auto found = entries_.find(ImageKey(stem));
        if (found == entries_.end())
            return {};
        const auto& hit = found->second.front();
        const auto& archive = archives_[hit.archive];
        const auto blob = Read(hit);
        auto plain = CompressedIwi6ToDds(blob);
        const bool wavelet = !plain && IsWaveletIwi6(blob);
        if (!plain && !wavelet)
            return {};
        if (wavelet && (!binary_ || !fs::is_regular_file(*binary_)))
            throw TextureError("wavelet image needs WaW bin/AssetViewer.exe (--waw-mod-tools)");
        const auto path = Destination(root_, stem, wavelet);
        if (wavelet)
        {
            if (!tables_)
                tables_ = LoadWaveletTables(*binary_);
            plain = WaveletToDds(blob, *tables_);
        }
        files::WriteBytes(path, *plain);
        const RecoveredImage image{path, archive.path, archive.contents->Entries()[hit.entry].name,
                                   static_cast<unsigned char>(blob[4]), wavelet ? binary_ : std::nullopt};
        recovered_.emplace(stem, image);
        return image;
    }
}
