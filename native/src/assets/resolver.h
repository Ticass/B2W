#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

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

    // the dependency closure of the requested assets
    struct AssetGraph
    {
        py::Json report;                // {nodes, edges, roots, missing}: the *.dependencies.json document
        std::vector<Path> roots;        // the source roots, then the stock roots the closure used
        std::vector<StockRoot> stockRoots;
    };

    // Stock dumps may expand lazily during traversal. The provider returns the
    // generation that actually contains the requested type. base: the converter's
    // resource root (the OAT field headers).
    AssetGraph Expand(const Path& base, const std::vector<Path>& sourceRoots, const AssetSet& requests, StockLookup lookup = {},
                      StockSounds sounds = {});
}
