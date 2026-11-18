#pragma once
// OpenAssetTools' Unlinker linked into the converter: the same zone loading and
// dumping code as Unlinker.exe, run in this process instead of a child process.

#include <optional>
#include <string>
#include <vector>

namespace w2b::oat
{
    struct UnlinkerRun
    {
        int exitCode = 0; // what Unlinker.exe returns: 0 success, 1 failure
        std::string log;  // what it prints to stdout and stderr, in order
    };

    // Runs the Unlinker with the arguments that follow the program name on the
    // Unlinker.exe command line (e.g. {"--no-color", "--list", "zone.ff"}).
    // OAT keeps global state, so one run at a time: this waits for a running one.
    UnlinkerRun RunUnlinker(const std::vector<std::string>& arguments);
    // the same, unless another run is in progress (nullopt then)
    std::optional<UnlinkerRun> TryRunUnlinker(const std::vector<std::string>& arguments);

    // what the linked code is: the OAT commit and a hash of the converter's
    // patch to it; extraction caches are keyed on it
    const char* Version();
} // namespace w2b::oat
