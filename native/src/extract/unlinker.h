#pragma once
// Every OpenAssetTools Unlinker command line goes through here. The WaW OAT
// (vendor/OpenAssetTools) is linked into the converter and runs in this
// process; another unlinker (the BO2 one) still runs as a child process.

#include "core/process.h"

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::extract
{
    // whether `tool` is the Unlinker.exe of the linked OAT checkout
    bool IsLinkedUnlinker(const std::filesystem::path& tool);

    // what an extraction cache records as the unlinker: the linked OAT's
    // version, else the tool file's sha256 (a rebuilt tool is then a new one)
    std::string UnlinkerIdentity(const std::filesystem::path& tool);

    // runs `command` (the unlinker path, then its arguments): in process when
    // it is the linked OAT and no other in-process run is busy, else as a child
    // process; one merged output either way (onLine sees it after an in-process run)
    process::Result RunUnlinker(const std::vector<std::string>& command, void (*onLine)(const std::string&) = nullptr);
} // namespace w2b::extract
