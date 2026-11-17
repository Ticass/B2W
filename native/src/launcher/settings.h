#pragma once
// Port of launcher.py's Settings, discovery and naming rules: the state the
// desktop launcher keeps in %LOCALAPPDATA%\WawConverter\settings.json.

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::launcher
{
    namespace fs = std::filesystem;

    struct Settings
    {
        std::string waw, bo2, waw_tools, t4, t6, decoder, work;
        std::string fastfile, project;
        std::string menu_title, menu_description, menu_mode, menu_location;
        std::string menu_blit, menu_large, menu_blur, menu_loading, menu_icon, menu_location_point, menu_preview;
        std::string menu_longitude = "0", menu_latitude = "0", loading_song;
        bool source_fx = false, bo2_stock_perks = false, remaster = false, redump = false, resume = false, verbose = false;

        Settings();
        // dataclasses.asdict order
        py::Json ToJson() const;
        static Settings FromJson(const py::Json& data);
        static Settings Load(const std::optional<fs::path>& path = std::nullopt);
        void Save(const std::optional<fs::path>& path = std::nullopt) const;

        // the string fields by name (the UI binds its edits to these)
        std::string* Text(const std::string& name);
        bool* Flag(const std::string& name);
        static const std::vector<std::string>& TextFields();
        static const std::vector<std::string>& FlagFields();
    };

    fs::path UserDirectory();
    // The folder holding vendor/ and tools/ (the checkout, or the portable folder)
    fs::path ResourceRoot();
    Settings Discover(Settings settings);
    std::vector<fs::path> MapFastfiles(const fs::path& folder);
    fs::path MapZone(const fs::path& source);
    std::string ProjectName(const std::string& source);
    // empty when valid, else the message launcher.validate_project raises
    std::string ValidateProject(const std::string& name);
    std::string Lower(std::string text);
    std::string Utf8(const fs::path& path);
    fs::path Path(const std::string& utf8);
} // namespace w2b::launcher
