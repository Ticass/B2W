#pragma once
// The preflight, BuildPaths, ProcessRunner and the build,
// extract, install and launch flows the desktop UI starts.

#include "settings.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace w2b::launcher
{
    struct Check
    {
        std::string name;
        bool ready = false;
        std::string detail;
        bool required = true;
    };

    struct CachePaths;
    // caches: the shared asset caches to check (default: the settings' own)
    std::vector<Check> Preflight(const Settings& settings, bool includeMap = true, const CachePaths* caches = nullptr);

    struct CachePaths
    {
        fs::path waw, bo2;
        static CachePaths ForSettings(const Settings& settings);
    };

    struct BuildPaths
    {
        fs::path root, stage, stock, wawDumps, mod, output;
        // throws std::invalid_argument for an invalid map name
        static BuildPaths ForSettings(const Settings& settings);
        bool Complete(const std::string& project, const bool* bo2StockPerks = nullptr) const;
    };

    std::vector<std::string> BuildCommand(const Settings& settings);
    // waw2bo2n.exe beside the launcher, else the checkout's native/build one
    std::vector<std::string> NativeCommand(const std::vector<std::string>& args);

    // Streams a process into a log file and the UI; Cancel ends its whole tree.
    class ProcessRunner
    {
    public:
        using Emit = std::function<void(const std::string& line)>;
        explicit ProcessRunner(Emit emit, bool verbose = false, int heartbeatSeconds = 15);
        ~ProcessRunner();
        int Run(const std::vector<std::string>& command, const fs::path& log);
        void Cancel();
        bool Cancelled() const
        {
            return cancelled;
        }
        void Line(const std::string& line)
        {
            emit(line);
        }

    private:
        Emit emit;
        bool verbose;
        int heartbeatSeconds;
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        void* job = nullptr;
    };

    // these throw std::runtime_error with the message the UI shows
    int PerformBuild(const Settings& settings, ProcessRunner& runner);
    int PerformExtractAll(const Settings& settings, ProcessRunner& runner);
    int PerformInstall(const Settings& settings, ProcessRunner& runner);
    bool Installed(const Settings& settings);
    void LaunchMap(const Settings& settings);
    util::Json InputStamp(const Settings& settings);

    fs::path PlutoniumRoot();
} // namespace w2b::launcher
