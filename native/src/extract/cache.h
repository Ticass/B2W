#pragma once
// Port of all2raw.py: resumable, shared extraction caches of game zones made
// with the OAT unlinkers. Receipts and cache keys are byte-compatible with the
// Python implementation (json.dumps(sort_keys=True) hashed with SHA-256), so
// both read each other's caches.

#include "core/pytext.h"

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

    // a failure the Python code raises; kind is its exception class
    struct Error : std::runtime_error
    {
        Error(std::string kind, const std::string& message) : std::runtime_error(message), kind(std::move(kind)) {}
        std::string kind;
    };

    std::string Key(const py::Json& data);              // all2raw.key
    std::string Digest(const fs::path& path);           // sha256 of the file
    py::Json Stamp(const fs::path& path);               // [size, mtime_ns]
    std::string Str(fs::path path);                     // str(Path)
    fs::path Resolve(const fs::path& path);             // Path.resolve()
    std::string SearchPaths(const std::vector<fs::path>& paths);
    void Publish(const fs::path& source, const fs::path& target, bool overwrite = false);
    void WriteJson(const fs::path& path, const py::Json& data);
    py::Json ReadJson(const fs::path& path);

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
    py::Json ListingJson(const std::string& text, bool references = false);

    struct Extraction
    {
        fs::path folder;
        py::Json data; // {"inputs", "files", "index", "loaded_index"}
    };

    struct ZoneRequest
    {
        fs::path ff, tool, root;
        std::string search;
        std::optional<std::string> assets;
        std::string imageFormat;
        std::optional<std::string> toolDigest;
        py::Json dependencies; // null or an object
        bool refresh = false, list = true;
    };
    Extraction ExtractZone(const ZoneRequest& request);

    // all2raw.parallel_zones: results in input order
    std::vector<py::Json> ParallelZones(const std::vector<fs::path>& zones, const std::function<py::Json(const fs::path&)>& extract, const std::string& label,
                                        std::optional<int> workers = std::nullopt);

    std::optional<fs::path> RequiredBo2Image(const fs::path& stock, const std::string& name, const std::optional<fs::path>& tool = std::nullopt);
    std::optional<fs::path> RequiredBo2Techset(const fs::path& stock, const std::string& name, const std::optional<fs::path>& tool = std::nullopt);
    fs::path ResourceRoot();
} // namespace w2b::extract
