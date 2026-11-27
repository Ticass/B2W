#pragma once
// The game caches: the stock WaW/BO2 views every build reads
// (prepare / ready), the map's own zone dumps (source_dumps) and the T6 script
// API cache (t6api.build with the shipped snapshot).

#include "cache.h"

#include <map>

namespace w2b::extract
{
    std::string StockAssets(const std::string& engine, const fs::path& ff);
    std::vector<fs::path> StockZones(const fs::path& game, const std::string& engine);
    util::Json Inventory(const fs::path& game, const fs::path& tool, const std::string& engine);
    // throws Error("RuntimeError", ...) when the cache must be (re)extracted
    fs::path Ready(const fs::path& game, const fs::path& tool, const fs::path& root, const std::string& engine);
    fs::path Prepare(const fs::path& game, const fs::path& tool, const fs::path& root, const std::string& engine, bool refresh = false,
                     std::optional<int> workers = std::nullopt);

    struct MapSettings
    {
        fs::path fastfile, waw, t4Tool;
    };
    // role ("map", "patch", "mod", "extra:<stem>") -> dump folder, in role order
    // converter: the build extracting now, stamped on fresh dumps; extractedBy: zone file -> the build that
    // extracted the dump in use ("" when it predates the stamps)
    std::vector<std::pair<std::string, fs::path>> SourceDumps(const MapSettings& settings, const fs::path& wawCache, bool refresh = false,
                                                              const std::string& converter = "", std::map<std::string, std::string>* extractedBy = nullptr);
    void RequireWawExtractor(const fs::path& tool);

    // t6api.build(game, cache, unlinker, stock_dump=dump) from the shipped snapshot
    void BuildT6ApiCache(const fs::path& snapshot, const fs::path& cache, bool withAssets, const fs::path& stockDump);
} // namespace w2b::extract
