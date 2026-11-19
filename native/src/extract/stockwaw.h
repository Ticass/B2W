#pragma once
// Port of wawassets.StockWawAssets: the stock WaW zones of the install, their
// asset index and dumps of the zones that define assets a map only references.

#include "core/pytext.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace w2b::extract
{
    namespace fs = std::filesystem;

    class StockWaw
    {
    public:
        StockWaw(fs::path wawRoot, fs::path unlinker, fs::path work);

        void Load();
        fs::path ZoneDir() const;
        const py::Json& Index() const
        {
            return index;
        }
        // zones defining (kind, name), case-insensitive, zombie zones first
        std::vector<std::string> ZonesDefining(const std::string& kind, const std::string& name) const;
        fs::path Dump(const std::string& zone, const std::optional<std::string>& kind = std::nullopt);
        // (zone, dump root) of the preferred zone defining the asset
        std::optional<std::pair<std::string, fs::path>> RootFor(const std::string& kind, const std::string& name);

        fs::path wawRoot, unlinker, work;

    private:
        py::Json index = py::Json::object(); // zone -> type -> names
        std::map<std::string, fs::path> prepared;
        std::map<std::string, py::Json> preparedSources;
        mutable std::map<std::pair<std::string, std::string>, std::vector<std::string>> lookup;
    };
} // namespace w2b::extract
