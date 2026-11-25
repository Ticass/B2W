#pragma once
// t6api.T6Api: what the BO2 script runtime provides (builtins with arity,
// stock script functions), loaded from the shipped snapshot and the stage's
// cache (stock assets), as t6api.build loads them.

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace w2b::gsc::port
{
    struct Range
    {
        int lo = 0, hi = 0;
    };

    class T6Api
    {
    public:
        // snapshot: compat/t6_script_api.json; cache: the stage's t6api_cache.json
        static T6Api Load(const std::filesystem::path& snapshot, const std::filesystem::path& cache);

        bool Defines(const std::string& script, const std::string& name) const;
        // method: nullopt for either kind (the union of both ranges)
        std::optional<Range> BuiltinRange(const std::string& name, std::optional<bool> method = std::nullopt) const;
        std::optional<bool> BuiltinAccepts(const std::string& name, int argc, std::optional<bool> method = std::nullopt) const;
        const std::vector<std::string>& ByName(const std::string& name) const;

        std::unordered_map<std::string, Range> builtins, methods;             // server
        std::unordered_map<std::string, Range> clientBuiltins, clientMethods; // client (t6zm.exe tables 0xD3AF00, 0xC62CA8)
        std::vector<std::pair<std::string, std::map<std::string, int>>> scripts; // file order
        std::unordered_map<std::string, size_t> scriptIndex;                     // lower path -> scripts index
        std::unordered_map<std::string, std::vector<std::string>> byName;
        std::map<std::string, std::set<std::string>> stockAssets;
    };
} // namespace w2b::gsc::port
