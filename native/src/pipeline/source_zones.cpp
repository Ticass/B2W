#include "source_zones.h"

#include "core/files.h"
#include "modutil.h"

#include <algorithm>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;
    using util::Json;

    std::optional<fs::path> Fastfile(const fs::path& root)
    {
        const auto extraction = root / "extraction.json";
        if (!mod::IsFile(extraction))
            return std::nullopt;
        const auto data = mod::Json(extraction);
        const auto file = data.value("inputs", Json::object()).value("file", std::string());
        const auto path = files::FromUtf8(file);
        return file.empty() || !mod::IsFile(path) ? std::nullopt : std::optional(path);
    }

    std::vector<fs::path> SourceZones(const std::vector<fs::path>& roots)
    {
        std::vector<fs::path> zones;
        for (const auto& root : roots)
            if (const auto file = Fastfile(root); file && std::ranges::find(zones, *file) == zones.end())
                zones.push_back(*file);
        return zones;
    }
} // namespace w2b::pipeline
