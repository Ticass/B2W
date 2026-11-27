#pragma once
// Resumable, shared extraction caches of game zones made
// with the OAT unlinkers. A cache key is the SHA-256 of the key's sorted
// compact JSON, so existing caches stay valid.

#include "core/text.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace w2b::extract
{
    namespace fs = std::filesystem;

    inline constexpr int SCHEMA = 3;
    extern const char* const WAW_ASSETS;

    // a step failure; kind names the failure class
    struct Error : std::runtime_error
    {
        Error(std::string kind, const std::string& message) : std::runtime_error(message), kind(std::move(kind)) {}
        std::string kind;
    };

    std::string Key(const util::Json& data);              // all2raw.key
    std::string Digest(const fs::path& path);           // sha256 of the file
    util::Json Stamp(const fs::path& path);               // [size, mtime_ns]
    std::string Str(fs::path path);                     // str(Path)
    fs::path Resolve(const fs::path& path);             // Path.resolve()
    std::string SearchPaths(const std::vector<fs::path>& paths);
    void Publish(const fs::path& source, const fs::path& target, bool overwrite = false);
    void WriteJson(const fs::path& path, const util::Json& data);
    util::Json ReadJson(const fs::path& path);

    // all2raw.cache_lock: an exclusive non-blocking lock held while alive
    class CacheLock
    {
    public:
        explicit CacheLock(const fs::path& root);
        ~CacheLock();
        CacheLock(const CacheLock&) = delete;
        CacheLock& operator=(const CacheLock&) = delete;

    private:
        void* handle = nullptr;
    };

    // all2raw.run: a native tool with its log, crash retries, failure tail
    void RunTool(const std::vector<std::string>& command, const fs::path& log);
    std::map<std::string, std::vector<std::string>> Listing(const std::string& text, bool references = false);
    util::Json ListingJson(const std::string& text, bool references = false);

    struct Extraction
    {
        fs::path folder;
        util::Json data; // {"inputs", "files", "index", "loaded_index", "converter" (who extracted it, when known)}
    };

    struct ZoneRequest
    {
        fs::path ff, tool, root;
        std::string search;
        std::optional<std::string> assets;
        std::string imageFormat;
        std::optional<std::string> toolDigest;
        util::Json dependencies; // null or an object
        bool refresh = false, list = true;
        // the converter build extracting it ("Test build #214"), kept in the receipt so a later build can
        // tell the user these files predate it; empty for dumps nobody asks about
        std::string converter;
        // an earlier generation of this zone with a subset of `assets`: only
        // the kinds it lacks are extracted, its files and listing are reused
        std::optional<fs::path> base;
    };
    Extraction ExtractZone(const ZoneRequest& request);

    // all2raw.parallel_zones: results in input order
    std::vector<util::Json> ParallelZones(const std::vector<fs::path>& zones, const std::function<util::Json(const fs::path&)>& extract, const std::string& label,
                                        std::optional<int> workers = std::nullopt);

    std::optional<fs::path> RequiredBo2Image(const fs::path& stock, const std::string& name, const std::optional<fs::path>& tool = std::nullopt);
    std::optional<fs::path> RequiredBo2Techset(const fs::path& stock, const std::string& name, const std::optional<fs::path>& tool = std::nullopt);
    fs::path ResourceRoot();
} // namespace w2b::extract
