// Zones kept loaded in this process (see loaded_zones.h).
#include "oat/loaded_zones.h"

#include "Game/AutoSearchPaths.h"
#include "IObjLoader.h"
#include "ObjWriter.h"
#include "ObjWriting.h"
#include "ZoneLoading.h"

#include <algorithm>
#include <cctype>
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace w2b::oat
{
    namespace
    {
        namespace fs = std::filesystem;

        // Loaded zones take about 2.5 times their fastfile in memory: this many
        // fastfile bytes stay loaded (roughly 1.3 GB), least recently used out first.
        constexpr std::uintmax_t LOADED_FASTFILE_BUDGET = 512ull << 20;

        std::string Lower(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // the zones kept loaded, most recently used first
        class LoadedZones
        {
        public:
            LoadedZone& Get(const fs::path& fastfile, const std::vector<fs::path>& searchPaths)
            {
                const auto key = Lower(fs::absolute(fastfile).string());
                if (const auto found = std::ranges::find(zones_, key, &Entry::key); found != zones_.end())
                {
                    zones_.splice(zones_.begin(), zones_, found);
                    return *zones_.front().zone;
                }
                auto zone = std::make_unique<LoadedZone>(fastfile, searchPaths, shared_);
                std::uintmax_t loaded = zone->Size();
                for (const auto& entry : zones_)
                    loaded += entry.zone->Size();
                while (!zones_.empty() && loaded > LOADED_FASTFILE_BUDGET)
                {
                    loaded -= zones_.back().zone->Size();
                    zones_.pop_back();
                }
                zones_.push_front({key, std::move(zone)});
                return *zones_.front().zone;
            }

        private:
            struct Entry
            {
                std::string key;
                std::unique_ptr<LoadedZone> zone;
            };
            // declared first: the zones unref their search paths when destroyed
            SharedSearchPaths shared_;
            std::list<Entry> zones_;
        };
    } // namespace

    LoadedZone::LoadedZone(const fs::path& fastfile, const std::vector<fs::path>& searchPaths, SharedSearchPaths& shared)
        : shared_(shared), size_(fs::file_size(fastfile))
    {
        auto loaded = ZoneLoading::LoadZone(fastfile.string(), std::nullopt);
        if (!loaded)
            throw std::runtime_error("Failed to load zone \"" + fastfile.string() + "\": " + loaded.error());
        zone_ = std::move(*loaded);
        for (const auto& path : searchPaths)
            paths_.push_back(path.string());
        for (const auto& path : AutoSearchPaths::GetForGame(zone_->m_game_id)->GetSearchPathsForZonePath(fs::absolute(fastfile).remove_filename().string()))
            paths_.push_back(path);
        for (const auto& path : paths_)
            shared_.RefSearchPath(path);
        IObjLoader::GetObjLoaderForGame(zone_->m_game_id)->LoadReferencedContainersForZone(shared_.GetSearchPath(), *zone_);
    }

    LoadedZone::~LoadedZone()
    {
        reading_.reset();
        IObjLoader::GetObjLoaderForGame(zone_->m_game_id)->UnloadContainersOfZone(*zone_);
        for (const auto& path : paths_)
            shared_.UnrefSearchPath(path);
    }

    bool LoadedZone::Dump(const std::vector<std::pair<std::string, std::string>>& assets, const fs::path& output)
    {
        const auto* game = IGame::GetGameById(zone_->m_game_id);
        ObjWriting::Configuration = {};
        ObjWriting::Configuration.ImageOutputFormat = ImageOutputFormat_e::DDS;
        ObjWriting::Configuration.ModelOutputFormat = ModelOutputFormat_e::GLTF;
        ObjWriting::Configuration.AssetTypesToHandleBitfield = std::vector(game->GetAssetTypeCount(), false);
        std::unordered_map<asset_type_t, std::unordered_set<std::string>> names;
        for (const auto& [kind, name] : assets)
        {
            const auto type = game->FindAssetTypeByName(kind);
            if (!type)
                throw std::runtime_error("unknown asset type \"" + kind + "\"");
            ObjWriting::Configuration.AssetTypesToHandleBitfield[*type] = true;
            names[*type].insert(Lower(name));
        }
        fs::create_directories(output);
        OutputPathFilesystem outputPath(output);
        const auto base = output.string();
        AssetDumpingContext context(*zone_, base, outputPath, shared_.GetSearchPath(), std::nullopt);
        context.SetAssetNameFilter(std::move(names));
        return IObjWriter::GetObjWriterForGame(zone_->m_game_id)->DumpZone(context);
    }

    AssetDumpingContext& LoadedZone::ReadingContext()
    {
        if (!reading_)
        {
            // nothing is written through it; the folder is never created
            readingBase_ = (fs::temp_directory_path() / "w2b_oat_reading").string();
            readingOutput_ = std::make_unique<OutputPathFilesystem>(readingBase_);
            reading_ = std::make_unique<AssetDumpingContext>(*zone_, readingBase_, *readingOutput_, shared_.GetSearchPath(), std::nullopt);
        }
        return *reading_;
    }

    LoadedZone& Loaded(const fs::path& fastfile, const std::vector<fs::path>& searchPaths)
    {
        // never destroyed: unloading at exit would race OAT's own static state
        static auto* zones = new LoadedZones();
        return zones->Get(fastfile, searchPaths);
    }
} // namespace w2b::oat
