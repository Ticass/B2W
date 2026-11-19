#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>

namespace w2b::paths
{
    using Path = std::filesystem::path;
    using py::Json;
    using Names = std::set<std::string, std::less<>>;
    using Alias = std::optional<std::string>;

    inline constexpr int NegotiationBegin = 16;
    inline constexpr int T6NegotiationBegin = 17;
    inline constexpr int ClearanceLinkFlags = 0x28;
    inline constexpr int MaxNodes = 0xFFFF - 128;
    inline constexpr std::string_view Prefix = "waw2bo2_";
    inline constexpr std::string_view Mantle = "zm_mantle_over_40";

    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct DogTraversals
    {
        Json calls = Json::object();
        std::string library;
        Json states = Json::object();
        std::vector<std::string> errors;
    };

    using Renames = std::map<std::string, std::string, std::less<>>;

    struct StagedPaths
    {
        Json summary;    // nodes, links, vis_bytes, traversals, scripts, generated
        Renames renamed; // WaW traversal animscript (or #node) -> the T6 script it plays
        DogTraversals dogs;
    };

    std::string ReadSource(const Path& file);
    Json ReadManifest(const Path& file);
    Names TraverseSubstates(std::string_view text);
    Names SourceScripts(const Json& manifest);
    DogTraversals SourceDogs(const std::vector<Path>& roots, const Names& scripts);
    std::string TraverseScript(const Alias& alias, const Json& dogCalls);
    Json Convert(const Json& source, const std::optional<Names>& substates, const Json& dogCalls);
    StagedPaths Stage(const Path& input, const Path& bsp, const Path& project, const std::optional<Path>& asd, const std::vector<Path>& roots);
    using WindowHubs = std::map<std::string, std::string, std::less<>>;
    WindowHubs SplitWindowEnds(Json& graph, const std::vector<std::size_t>& begins);
    Json LinkWindowTraversals(const Path& bsp);
}
