#pragma once
// discover_animation_dependencies, port_map, the linker, fix_arity and
// link_check.

#include "report.h"
#include "sources.h"
#include "t6api.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::gsc::port
{
    // actor kind -> T6 actor template source ("human" for a plain template)
    using ActorTemplates = std::map<std::string, std::string>;

    struct PortMapOptions
    {
        std::map<std::string, std::string> fxTable;
        std::set<std::string> animtrees;
        std::map<std::string, std::set<std::string>> coreAnimtrees;
        bool bo2StockPerks = false;
        std::set<std::string> actorTypes;
        std::optional<ActorTemplates> actorTemplate;
        bool stockMap = false;
        bool clientPorted = false;
    };

    std::map<std::string, std::set<std::string>> DiscoverAnimationDependencies(const Sources& sources, const T6Api& api, const std::string& mapName,
                                                                               const std::set<std::string>& actorTypes = {});
    PortReport PortMap(const Sources& sources, const T6Api& api, const std::string& mapName, const std::filesystem::path& outRoot,
                       const PortMapOptions& options = {});
    int FixArity(const std::filesystem::path& outRoot, const T6Api& api, PortReport* report = nullptr);
    std::vector<std::string> LinkCheck(const std::filesystem::path& outRoot, const T6Api& api);

    // copy.deepcopy(sources): private tables and parsed scripts
    Sources DeepCopy(const Sources& sources);
} // namespace w2b::gsc::port
