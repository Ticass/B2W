#include "MapEntsLinker.h"

#include "Game/T6/BSP/BSPUtil.h"

#include <nlohmann/json.hpp>

using namespace nlohmann;
using namespace T6;

namespace
{
    bool parseMapEntsJSON(json& entArrayJs, std::string& entityString)
    {
        for (size_t entIdx = 0; entIdx < entArrayJs.size(); entIdx++)
        {
            auto& entity = entArrayJs[entIdx];

            if (entIdx == 0)
            {
                std::string className;
                entity.at("classname").get_to(className);
                if (className != "worldspawn")
                {
                    con::error("ERROR: first entity in the map entity string must be the worldspawn class!");
                    return false;
                }
            }

            entityString.append("{\n");

            for (auto& element : entity.items())
            {
                std::string key = element.key();
                std::string value = element.value();
                entityString.append(std::format("\"{}\" \"{}\"\n", key, value));
            }

            entityString.append("}\n");
        }

        return true;
    }

    void parseSpawnpointJSON(json& entArrayJs, std::string& entityString, const char* spawnpointNames[], size_t nameCount)
    {
        for (auto& element : entArrayJs.items())
        {
            std::string origin;
            std::string angles;
            auto& entity = element.value();
            entity.at("origin").get_to(origin);
            entity.at("angles").get_to(angles);

            for (size_t nameIdx = 0; nameIdx < nameCount; nameIdx++)
            {
                entityString.append("{\n");
                entityString.append(std::format("\"origin\" \"{}\"\n", origin));
                entityString.append(std::format("\"angles\" \"{}\"\n", angles));
                entityString.append(std::format("\"classname\" \"{}\"\n", spawnpointNames[nameIdx]));
                entityString.append("}\n");
            }
        }
    }
} // namespace

namespace BSP
{
    MapEntsLinker::MapEntsLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context)
        : m_memory(memory),
          m_search_path(searchPath),
          m_context(context)
    {
    }

    // Trigger entities ("model" "*N") are tested against MapEnts trigger data:
    // model N-1 -> one hull per brush (its AABB) -> one slab per non-axial side.
    void MapEntsLinker::LinkTriggers(MapEnts& mapEnts) const
    {
        mapEnts.trigger = {};
        const auto subFile = m_search_path.Open(GetFileNameForBSPAsset("submodels.json"));
        if (!subFile.IsOpen())
            return;

        std::vector<TriggerModel> models;
        std::vector<TriggerHull> hulls;
        std::vector<TriggerSlab> slabs;
        for (const auto& modelJs : json::parse(*subFile.m_stream).at("submodels"))
        {
            TriggerModel model{};
            model.firstHull = static_cast<uint16_t>(hulls.size());
            for (const auto& brushJs : modelJs.at("brushes"))
            {
                TriggerHull hull{};
                const auto& mins = brushJs.at("mins");
                const auto& maxs = brushJs.at("maxs");
                for (auto axis = 0; axis < 3; axis++)
                {
                    const auto lo = mins.at(axis).get<float>();
                    const auto hi = maxs.at(axis).get<float>();
                    hull.bounds.midPoint.v[axis] = (lo + hi) * 0.5f;
                    hull.bounds.halfSize.v[axis] = (hi - lo) * 0.5f;
                }
                hull.contents = brushJs.at("contents").get<int>();
                hull.firstSlab = static_cast<uint16_t>(slabs.size());
                const auto& verts = brushJs.at("verts");
                for (const auto& side : brushJs.at("sides"))
                {
                    if (verts.empty())
                        break;
                    TriggerSlab slab{};
                    slab.dir.x = side.at(0).get<float>();
                    slab.dir.y = side.at(1).get<float>();
                    slab.dir.z = side.at(2).get<float>();
                    const auto hi = side.at(3).get<float>();
                    auto lo = hi;
                    for (const auto& v : verts)
                        lo = std::min(lo, slab.dir.x * v.at(0).get<float>() + slab.dir.y * v.at(1).get<float>() + slab.dir.z * v.at(2).get<float>());
                    slab.midPoint = (lo + hi) * 0.5f;
                    slab.halfSize = (hi - lo) * 0.5f;
                    slabs.emplace_back(slab);
                }
                hull.slabCount = static_cast<uint16_t>(slabs.size() - hull.firstSlab);
                model.contents |= hull.contents;
                hulls.emplace_back(hull);
            }
            model.hullCount = static_cast<uint16_t>(hulls.size() - model.firstHull);
            models.emplace_back(model);
        }

        mapEnts.trigger.count = static_cast<unsigned int>(models.size());
        mapEnts.trigger.models = m_memory.Alloc<TriggerModel>(models.size());
        std::copy(models.begin(), models.end(), mapEnts.trigger.models);
        mapEnts.trigger.hullCount = static_cast<unsigned int>(hulls.size());
        mapEnts.trigger.hulls = m_memory.Alloc<TriggerHull>(hulls.size());
        std::copy(hulls.begin(), hulls.end(), mapEnts.trigger.hulls);
        mapEnts.trigger.slabCount = static_cast<unsigned int>(slabs.size());
        mapEnts.trigger.slabs = m_memory.Alloc<TriggerSlab>(slabs.size());
        std::copy(slabs.begin(), slabs.end(), mapEnts.trigger.slabs);
        con::info("Linked {} trigger models, {} hulls, {} slabs", models.size(), hulls.size(), slabs.size());
    }

    MapEnts* MapEntsLinker::LinkMapEnts(const BSPData& bsp) const
    {
        try
        {
            json entJs;
            const auto entityFilePath = GetFileNameForBSPAsset("entities.json");
            const auto entFile = m_search_path.Open(entityFilePath);
            if (!entFile.IsOpen())
            {
                con::warn("Can't find entity file {}, using default entities instead", entityFilePath);
                entJs = json::parse(BSPLinkingConstants::DEFAULT_MAP_ENTS_STRING);
            }
            else
            {
                entJs = json::parse(*entFile.m_stream);
            }
            std::string entityString;
            if (!parseMapEntsJSON(entJs["entities"], entityString))
                return nullptr;

            json spawnJs;
            const auto spawnFilePath = GetFileNameForBSPAsset("spawns.json");
            const auto spawnFile = m_search_path.Open(spawnFilePath);
            if (!spawnFile.IsOpen())
            {
                con::warn("Cant find spawn file {}, setting spawns to 0 0 0", spawnFilePath);
                spawnJs = json::parse(BSPLinkingConstants::DEFAULT_SPAWN_POINT_STRING);
            }
            else
            {
                spawnJs = json::parse(*spawnFile.m_stream);
            }

            constexpr auto defenderNameCount = std::extent_v<decltype(BSPGameConstants::DEFENDER_SPAWN_POINT_NAMES)>;
            constexpr auto attackerNameCount = std::extent_v<decltype(BSPGameConstants::ATTACKER_SPAWN_POINT_NAMES)>;
            constexpr auto ffaNameCount = std::extent_v<decltype(BSPGameConstants::FFA_SPAWN_POINT_NAMES)>;

            parseSpawnpointJSON(spawnJs["attackers"], entityString, BSPGameConstants::DEFENDER_SPAWN_POINT_NAMES, defenderNameCount);
            parseSpawnpointJSON(spawnJs["defenders"], entityString, BSPGameConstants::ATTACKER_SPAWN_POINT_NAMES, attackerNameCount);
            parseSpawnpointJSON(spawnJs["FFA"], entityString, BSPGameConstants::FFA_SPAWN_POINT_NAMES, ffaNameCount);

            MapEnts* mapEnts = m_memory.Alloc<MapEnts>();
            mapEnts->name = m_memory.Dup(bsp.bspName.c_str());

            mapEnts->entityString = m_memory.Dup(entityString.c_str());
            mapEnts->numEntityChars = static_cast<int>(entityString.length() + 1); // numEntityChars includes the null character

            LinkTriggers(*mapEnts);

            return mapEnts;
        }
        catch (const json::exception& e)
        {
            con::error("JSON error when parsing map ents and spawns: {}", e.what());
            return nullptr;
        }
    }
} // namespace BSP
