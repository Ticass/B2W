#include "JsonLoaderImpactFxT6.h"

#include "Game/T6/T6.h"
#include "Utils/Logging/Log.h"

#include <format>
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace nlohmann;
using namespace T6;

namespace
{
    constexpr auto ROWS = 21u;

    class ImpactFxLoader final : public AssetCreator<AssetImpactFx>
    {
    public:
        ImpactFxLoader(MemoryManager& memory, ISearchPath& searchPath)
            : m_memory(memory),
              m_search_path(searchPath)
        {
        }

        AssetCreationResult CreateAsset(const std::string& assetName, AssetCreationContext& context) override
        {
            const auto file = m_search_path.Open(std::format("fx/{}.w2bimpacts.json", assetName));
            if (!file.IsOpen())
                return AssetCreationResult::NoAction();

            auto* table = m_memory.Alloc<FxImpactTable>();
            table->name = m_memory.Dup("");
            table->table = m_memory.Alloc<FxImpactEntry>(ROWS);
            AssetRegistration<AssetImpactFx> registration("", table); // the writer finds an asset by its own name
            try
            {
                const auto j = json::parse(*file.m_stream);
                if (j.size() != ROWS)
                    throw std::runtime_error(std::format("{} rows, expected {}", j.size(), ROWS));
                for (auto row = 0u; row < ROWS; row++)
                {
                    auto& entry = table->table[row];
                    constexpr auto surfaces = std::extent_v<decltype(FxImpactEntry::nonflesh)>;
                    constexpr auto flesh = std::extent_v<decltype(FxImpactEntry::flesh)>;
                    if (j[row].size() != surfaces + flesh)
                        throw std::runtime_error(std::format("{} columns, expected {}", j[row].size(), surfaces + flesh));
                    for (auto column = 0u; column < surfaces + flesh; column++)
                    {
                        auto* effect = Effect(j[row][column].get<std::string>(), context, registration);
                        if (column < surfaces)
                            entry.nonflesh[column] = effect;
                        else
                            entry.flesh[column - surfaces] = effect;
                    }
                }
            }
            catch (const std::exception& e)
            {
                con::error("Failed to load impact effects \"{}\": {}", assetName, e.what());
                return AssetCreationResult::Failure();
            }

            return AssetCreationResult::Success(context.AddAsset(std::move(registration)));
        }

    private:
        static FxEffectDef* Effect(const std::string& name, AssetCreationContext& context, AssetRegistration<AssetImpactFx>& registration)
        {
            if (name.empty())
                return nullptr;
            auto* dependency = context.LoadDependency<AssetFx>(name);
            if (!dependency)
                throw std::runtime_error(std::format("could not load effect \"{}\"", name));
            registration.AddDependency(dependency);
            return dependency->Asset();
        }

        MemoryManager& m_memory;
        ISearchPath& m_search_path;
    };
} // namespace

namespace fx
{
    std::unique_ptr<AssetCreator<AssetImpactFx>> CreateJsonLoaderImpactT6(MemoryManager& memory, ISearchPath& searchPath)
    {
        return std::make_unique<ImpactFxLoader>(memory, searchPath);
    }
} // namespace fx
