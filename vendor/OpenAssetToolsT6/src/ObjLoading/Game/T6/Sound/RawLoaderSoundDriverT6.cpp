#include "RawLoaderSoundDriverT6.h"

#include "Game/T6/SoundDriverBinary.h"
#include "Utils/Logging/Log.h"

#include <cmath>
#include <format>
#include <set>

using namespace T6;

namespace
{
    template<typename T> void Read(std::istream& stream, MemoryManager& memory, unsigned int& count, T*& values, unsigned int limit)
    {
        unsigned int stride = 0;
        stream.read(reinterpret_cast<char*>(&count), sizeof(count));
        stream.read(reinterpret_cast<char*>(&stride), sizeof(stride));
        if (!stream || count > limit || stride != sizeof(T))
            throw std::runtime_error("invalid sound driver table count/stride");
        values = count ? memory.Alloc<T>(count) : nullptr;
        if (count)
            stream.read(reinterpret_cast<char*>(values), sizeof(T) * count);
        if (!stream)
            throw std::runtime_error("truncated sound driver table");
    }

    template<typename T> void CheckNames(const T* values, unsigned int count)
    {
        std::set<std::string> names;
        for (auto i = 0u; i < count; ++i)
        {
            if (!std::memchr(values[i].name, 0, sizeof(values[i].name)) || !values[i].name[0])
                throw std::runtime_error("empty or unterminated sound driver name");
            if (!names.emplace(values[i].name).second)
                throw std::runtime_error("duplicate sound driver name");
        }
    }

    class DriverLoader final : public AssetCreator<AssetSoundDriverGlobals>
    {
    public:
        DriverLoader(MemoryManager& memory, ISearchPath& searchPath) : m_memory(memory), m_search_path(searchPath) {}

        AssetCreationResult CreateAsset(const std::string& assetName, AssetCreationContext& context) override
        {
            const auto file = m_search_path.Open(std::format("sounddriverglobals/{}.w2bsdg", assetName));
            if (!file.IsOpen())
                return AssetCreationResult::NoAction();
            auto* driver = m_memory.Alloc<SndDriverGlobals>();
            driver->name = m_memory.Dup(assetName.c_str());
            try
            {
                auto& stream = *file.m_stream;
                char magic[8]{};
                stream.read(magic, sizeof(magic));
                if (!stream || std::memcmp(magic, sound_driver_binary::MAGIC, sizeof(magic)))
                    throw std::runtime_error("not a version 1 T6 sound driver sidecar");
                Read(stream, m_memory, driver->groupCount, driver->groups, 32);
                Read(stream, m_memory, driver->curveCount, driver->curves, 64);
                Read(stream, m_memory, driver->panCount, driver->pans, 64);
                Read(stream, m_memory, driver->duckGroupCount, driver->duckGroups, 32);
                Read(stream, m_memory, driver->contextCount, driver->contexts, 64);
                Read(stream, m_memory, driver->masterCount, driver->masters, 64);
                Read(stream, m_memory, driver->voiceDuckCount, driver->voiceDucks, 64);
                Read(stream, m_memory, driver->futzCount, driver->futzes, 64);
                if (stream.peek() != std::char_traits<char>::eof())
                    throw std::runtime_error("unexpected trailing sound driver data");
                CheckNames(driver->groups, driver->groupCount);
                CheckNames(driver->curves, driver->curveCount);
                CheckNames(driver->pans, driver->panCount);
                CheckNames(driver->duckGroups, driver->duckGroupCount);
                CheckNames(driver->masters, driver->masterCount);
                CheckNames(driver->voiceDucks, driver->voiceDuckCount);
                CheckNames(driver->futzes, driver->futzCount);
                for (auto i = 0u; i < driver->curveCount; ++i)
                    for (auto j = 0u; j < 8; ++j)
                    {
                        const auto& point = driver->curves[i].points[j];
                        if (!std::isfinite(point.x) || !std::isfinite(point.y) || (j && point.x < driver->curves[i].points[j - 1].x))
                            throw std::runtime_error("invalid sound driver curve points");
                    }
            }
            catch (const std::exception& e)
            {
                con::error("Failed to load sound driver \"{}\": {}", assetName, e.what());
                return AssetCreationResult::Failure();
            }
            AssetRegistration<AssetSoundDriverGlobals> registration(assetName, driver);
            return AssetCreationResult::Success(context.AddAsset(std::move(registration)));
        }

    private:
        MemoryManager& m_memory;
        ISearchPath& m_search_path;
    };
}

namespace sound
{
    std::unique_ptr<AssetCreator<AssetSoundDriverGlobals>> CreateRawDriverLoaderT6(MemoryManager& memory, ISearchPath& searchPath)
    {
        return std::make_unique<DriverLoader>(memory, searchPath);
    }
}
