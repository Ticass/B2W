#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <utility>

namespace w2b::assets
{
    using Path = std::filesystem::path;
    using Asset = std::pair<std::string, std::string>;
    using AssetSet = std::set<Asset>;
    using StockRoot = std::pair<std::string, Path>;
    using StockLookup = std::function<std::optional<StockRoot>(const Asset&)>;
    using StockSounds = std::function<std::set<std::string>()>;

    Path SourcePath(const Asset& asset);
    Path FindFile(const Path& root, const Path& relative, bool loadedSound);

    // Stock dumps may expand lazily during traversal. The provider returns the
    // generation that actually contains the requested type.
    py::Json Expand(const py::Json& request, StockLookup lookup = {}, StockSounds sounds = {});
}
