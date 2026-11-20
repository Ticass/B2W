#pragma once
// Sources: map scripts in WaW load priority plus the stock scripts.
// A copy shares the tables; code that replaces a table swaps the pointer.

#include "util.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace w2b::gsc::port
{
    // an insertion-ordered str -> str dict
    struct TextDict
    {
        std::vector<std::string> order;
        std::unordered_map<std::string, std::string> values;

        bool Has(const std::string& key) const
        {
            return values.count(key) != 0;
        }
        const std::string* Find(const std::string& key) const
        {
            const auto it = values.find(key);
            return it == values.end() ? nullptr : &it->second;
        }
        void Set(const std::string& key, std::string value)
        {
            if (!values.count(key))
                order.push_back(key);
            values[key] = std::move(value);
        }
    };

    using ScriptPtr = std::shared_ptr<Script>;

    class Sources
    {
    public:
        Sources(const std::vector<std::filesystem::path>& roots, const std::vector<std::filesystem::path>& iwds,
                const std::optional<std::filesystem::path>& stockRoot, const std::vector<std::filesystem::path>& variantRoots);

        ScriptPtr Get(const std::string& path, const std::string& ext = ".gsc");
        std::vector<std::string> StockVariants(const std::string& path, const std::string& ext = ".gsc");
        std::map<std::string, std::string> BindZombieFramework(const std::string& mainPath);
        bool IsStock(const std::string& path, const std::string& ext = ".gsc");

        // text.get(key, stock.get(key))
        const std::string* Source(const std::string& key) const;

        std::shared_ptr<TextDict> text, origin, stock;
        std::shared_ptr<std::unordered_map<std::string, ScriptPtr>> parsed;
        std::vector<std::filesystem::path> variantRoots;
    };

    // gscport.is_core
    bool IsCore(const std::string& path, Sources& sources);
} // namespace w2b::gsc::port
