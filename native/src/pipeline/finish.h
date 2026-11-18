#pragma once
// The build's last steps after staging: compile the map scripts with the BO2
// mod tools linker (t6bridge.compile_scripts), link the map zone with the T6
// bridge (cli._bridge_link) and assemble the Plutonium mod folder (package.py).

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    // a build step's error, printed as "waw2bo2: error: <kind>: <message>"
    struct StepError : std::runtime_error
    {
        StepError(std::string kind, const std::string& message) : std::runtime_error(message), kind(std::move(kind)) {}
        std::string kind;
    };

    std::vector<std::string> MapScripts(const fs::path& projectRoot, const std::string& project);
    void ValidateOwnedInitCalls(const fs::path& projectRoot, const std::string& project);
    // (compiled script names, linker log)
    std::pair<std::vector<std::string>, std::string> CompileScripts(const fs::path& stage, const std::string& project, const fs::path& bo2,
                                                                    const fs::path& unlinker);
    std::pair<std::vector<std::string>, std::vector<std::string>> ParseScriptLinkerLog(const std::string& log);
    std::vector<std::string> LinkerCommand(const fs::path& linker, const fs::path& stage, const std::string& project, const fs::path* techsetDump);
    // cli bridge-link: prints as the Python command does; returns its exit code
    int BridgeLink(const fs::path& stage, const std::string& project, const fs::path& linker, const fs::path* techsetDump, const fs::path* log);

    fs::path PlutoniumModsDir();
    fs::path Package(const std::string& project, const fs::path& mapOut, const fs::path& modOut, const fs::path* dest = nullptr);
    std::vector<std::string> LaunchCommand(const std::string& project, const fs::path& bo2, bool directMap = false);
    // " ".join(f'"{c}"' if " " in c else c ...)
    std::string QuotedJoin(const std::vector<std::string>& command);
} // namespace w2b::pipeline
