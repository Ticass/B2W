#pragma once
// Zones OpenAssetTools keeps loaded in this process, shared by every reader
// and dumper in w2b_oat. Internal to w2b_oat: it includes OAT headers. All
// calls hold StateLock().

#include "Dumping/AssetDumpingContext.h"
#include "SearchPath/OutputPathFilesystem.h"
#include "SearchPath/SharedSearchPaths.h"
#include "Zone/Zone.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace w2b::oat
{
    // a zone loaded the way the Unlinker loads one it dumps
    class LoadedZone
    {
    public:
        LoadedZone(const std::filesystem::path& fastfile, const std::vector<std::filesystem::path>& searchPaths, SharedSearchPaths& shared);
        ~LoadedZone();
        LoadedZone(const LoadedZone&) = delete;
        LoadedZone& operator=(const LoadedZone&) = delete;

        // what the Unlinker's dump does for this zone, for the named assets only
        bool Dump(const std::vector<std::pair<std::string, std::string>>& assets, const std::filesystem::path& output);

        const Zone& Get() const
        {
            return *zone_;
        }
        // a context for converting assets in memory: the zone state OAT's
        // dumpers build (constant and texture names) without writing files
        AssetDumpingContext& ReadingContext();

        std::uintmax_t Size() const
        {
            return size_;
        }

    private:
        SharedSearchPaths& shared_;
        std::unique_ptr<Zone> zone_;
        std::vector<std::string> paths_;
        std::uintmax_t size_;
        std::string readingBase_;
        std::unique_ptr<OutputPathFilesystem> readingOutput_;
        std::unique_ptr<AssetDumpingContext> reading_;
    };

    // the zone, loaded once and kept while it fits the memory budget
    LoadedZone& Loaded(const std::filesystem::path& fastfile, const std::vector<std::filesystem::path>& searchPaths);
} // namespace w2b::oat
