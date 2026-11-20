#pragma once
// WaW assets that exist only as WaW Mod Tools sources
// (GUIDELINES 27, source 4). An effect, material or model in no compiled zone
// of the WaW install is compiled from the Mod Tools raw/ folder with WaW's own
// linker (bin/linker_pc.exe) into a scratch fastfile, which the T4 unlinker
// then dumps like any other WaW zone; no source format is reimplemented.
// Opt-in (--waw-source-fx): it adds content the WaW build never showed, and
// every use is reported by the caller (WAW_SOURCE_ASSET).

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace w2b::extract
{
    namespace fs = std::filesystem;

    class WawSourceError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class WawSource
    {
    public:
        WawSource(fs::path modTools, fs::path wawRoot, fs::path unlinker, fs::path work);

        // the request form {mod_tools, waw_root, unlinker, work}; nullopt for null

        // the Mod Tools folder holding bin/linker_pc.exe: the explicit one, else
        // the WaW install (the installer's default) or <resources>/wawModTools
        static std::optional<fs::path> Locate(const std::optional<fs::path>& explicitFolder, const std::optional<fs::path>& wawRoot,
                                              const fs::path& resourceRoot);

        // what keeps the sources unusable (empty when they can be compiled)
        std::vector<std::string> Check() const;
        // the dump root holding the compiled asset and everything it
        // references; nullopt when the Mod Tools have no source for it
        std::optional<fs::path> Compile(const std::string& kind, const std::string& name);
        // the assets the linker could not load while compiling (missing sources)
        std::vector<std::string> LinkWarnings(const std::string& kind, const std::string& name) const;

        const fs::path& ModTools() const
        {
            return modTools_;
        }

    private:
        fs::path Raw() const;
        std::optional<fs::path> Source(const std::string& kind, const std::string& name) const;
        fs::path Workspace(const fs::path& raw) const;

        fs::path modTools_, wawRoot_, unlinker_, work_;
    };

    // linker_pc truncates zone names to 63 characters
    std::string SourceZoneName(const std::string& kind, const std::string& name);
} // namespace w2b::extract
