#pragma once
// Port of clientport.py: the map's own client scripts on the T6 client runtime.

#include "sources.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace w2b::gsc::port::clientport
{
    inline constexpr const char* PORT_HEADER = "// Translated from World at War by waw2bo2 (clientport).";
    inline constexpr const char* CLIENT_PREFIX = "clientscripts\\";
    inline constexpr const char* PORTED_PREFIX = "clientscripts\\mp\\waw\\";
    inline constexpr const char* CLIENT_GLUE = "clientscripts\\mp\\waw\\_waw2bo2_client";
    inline constexpr const char* ENVIRONMENT = "clientscripts\\mp\\waw\\_waw2bo2_environment";

    std::string TranslateLibrary(const std::string& source, const std::map<std::string, std::string>& effects,
                                 const std::map<std::string, std::string>& weapons, const std::set<std::string>& soundNames);
    std::string StageTesla(const std::vector<std::filesystem::path>& roots, const std::filesystem::path& project,
                           const std::map<std::string, std::string>& effects, const std::map<std::string, std::string>& weaponTable,
                           const std::set<std::string>& soundNames);
    std::map<std::string, std::string> StagedClientModules(const std::string& mapName, const std::string& project);
    std::string ClientNorm(const std::string& path);
    // the port report (an empty object when the map has no client main)
    py::Json PortMapClient(Sources& sources, const std::string& mapName, const std::filesystem::path& outRoot,
                           const std::map<std::string, std::string>& staged = {}, const std::set<std::string>& builtins = {},
                           const std::map<std::string, std::set<std::string>>& declared = {});
    std::string FinishMapClient(const std::filesystem::path& projectRoot, py::Json& report, const std::map<std::string, std::string>& effects,
                                const std::map<std::string, std::string>& weapons, const std::set<std::string>& soundNames);
} // namespace w2b::gsc::port::clientport
