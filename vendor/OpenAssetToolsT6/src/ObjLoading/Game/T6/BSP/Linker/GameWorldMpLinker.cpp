#include "GameWorldMpLinker.h"

#include "Game/T6/BSP/BSPUtil.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>

using namespace T6;
using namespace nlohmann;

namespace
{
    // Stock zm_nuked keeps 128 spare nodes after the map's nodes (runtime-created nodes).
    constexpr unsigned EXTRA_NODE_COUNT = 128u;
    // Stock trees split until a handful of nodes remain.
    constexpr size_t MAX_TREE_LEAF_NODES = 4u;

    char ByteFromHex(const char high, const char low)
    {
        const auto nibble = [](const char c) -> int
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            throw std::runtime_error("invalid hex digit in pathVis");
        };
        return static_cast<char>((nibble(high) << 4) | nibble(low));
    }
} // namespace

namespace BSP
{
    GameWorldMpLinker::GameWorldMpLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context, Zone& zone)
        : m_memory(memory),
          m_search_path(searchPath),
          m_context(context),
          m_zone(zone)
    {
    }

    scr_string_t GameWorldMpLinker::ScriptString(const std::string& value)
    {
        if (value.empty())
            return 0;
        const auto scrString = m_zone.m_script_strings.AddOrGetScriptString(value);
        m_script_strings.emplace_back(scrString);
        return scrString;
    }

    // kd-tree over node origins, as in stock zones: split on the wider of the
    // x/y extents at its midpoint; child[0] holds origins below the split,
    // child[1] the rest; child[1] directly follows its parent in the array.
    void GameWorldMpLinker::BuildNodeTree(PathData& path) const
    {
        struct TreeEntry
        {
            int axis;
            float dist;
            int child[2];
            std::vector<uint16_t> nodes;
        };
        std::vector<TreeEntry> entries;

        const std::function<int(std::vector<uint16_t>&)> build = [&](std::vector<uint16_t>& ids) -> int
        {
            const auto index = static_cast<int>(entries.size());
            entries.emplace_back();

            float mins[2] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            float maxs[2] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            for (const auto id : ids)
            {
                const auto& origin = path.nodes[id].constant.vOrigin;
                mins[0] = std::min(mins[0], origin.x);
                maxs[0] = std::max(maxs[0], origin.x);
                mins[1] = std::min(mins[1], origin.y);
                maxs[1] = std::max(maxs[1], origin.y);
            }
            const auto axis = maxs[1] - mins[1] > maxs[0] - mins[0] ? 1 : 0;
            const auto dist = (mins[axis] + maxs[axis]) * 0.5f;

            if (ids.size() <= MAX_TREE_LEAF_NODES || maxs[axis] - mins[axis] <= 0.0f)
            {
                entries[index].axis = -1;
                entries[index].nodes = ids;
                return index;
            }

            std::vector<uint16_t> below, above;
            for (const auto id : ids)
            {
                const auto& origin = path.nodes[id].constant.vOrigin;
                ((axis == 0 ? origin.x : origin.y) < dist ? below : above).emplace_back(id);
            }
            const auto aboveIndex = build(above);
            const auto belowIndex = build(below);
            entries[index].axis = axis;
            entries[index].dist = dist;
            entries[index].child[0] = belowIndex;
            entries[index].child[1] = aboveIndex;
            return index;
        };

        std::vector<uint16_t> all(path.nodeCount);
        for (auto i = 0u; i < path.nodeCount; i++)
            all[i] = static_cast<uint16_t>(i);
        if (all.empty())
            return;
        build(all);

        path.nodeTreeCount = static_cast<int>(entries.size());
        path.nodeTree = m_memory.Alloc<pathnode_tree_t>(entries.size());
        for (size_t i = 0; i < entries.size(); i++)
        {
            auto& tree = path.nodeTree[i];
            const auto& entry = entries[i];
            tree.axis = entry.axis;
            if (entry.axis < 0)
            {
                tree.dist = 0.0f;
                tree.u.s.nodeCount = static_cast<int>(entry.nodes.size());
                tree.u.s.nodes = m_memory.Alloc<uint16_t>(entry.nodes.size());
                std::copy(entry.nodes.begin(), entry.nodes.end(), tree.u.s.nodes);
            }
            else
            {
                tree.dist = entry.dist;
                tree.u.child[0] = &path.nodeTree[entry.child[0]];
                tree.u.child[1] = &path.nodeTree[entry.child[1]];
            }
        }
    }

    bool GameWorldMpLinker::LoadPaths(PathData& path)
    {
        const auto file = m_search_path.Open(GetFileNameForBSPAsset("paths.json"));
        if (!file.IsOpen())
        {
            con::warn("no BSP/paths.json: the map has no path nodes, AI cannot path");
            return true;
        }

        json js;
        try
        {
            js = json::parse(*file.m_stream);
        }
        catch (const json::exception& e)
        {
            con::error("failed to parse BSP/paths.json: {}", e.what());
            return false;
        }

        const auto& nodesJs = js.at("nodes");
        const auto nodeCount = static_cast<unsigned>(nodesJs.size());
        if (nodeCount + EXTRA_NODE_COUNT > std::numeric_limits<uint16_t>::max())
        {
            con::error("{} path nodes exceed the uint16 node index range", nodeCount);
            return false;
        }

        path.nodeCount = nodeCount;
        path.originalNodeCount = nodeCount;
        path.nodes = m_memory.Alloc<pathnode_t>(nodeCount + EXTRA_NODE_COUNT);
        path.basenodes = m_memory.Alloc<pathbasenode_t>(nodeCount + EXTRA_NODE_COUNT);

        size_t linkCount = 0;
        for (auto i = 0u; i < nodeCount; i++)
        {
            const auto& nodeJs = nodesJs[i];
            auto& node = path.nodes[i].constant;
            node.type = static_cast<nodeType>(nodeJs.at("type").get<int>());
            node.spawnflags = nodeJs.at("spawnflags").get<int>();
            node.targetname = ScriptString(nodeJs.at("targetname").get<std::string>());
            node.script_linkName = ScriptString(nodeJs.at("script_linkname").get<std::string>());
            node.script_noteworthy = ScriptString(nodeJs.at("script_noteworthy").get<std::string>());
            node.target = ScriptString(nodeJs.at("target").get<std::string>());
            node.animscript = ScriptString(nodeJs.at("animscript").get<std::string>());
            node.animscriptfunc = nodeJs.at("animscriptfunc").get<int>();
            const auto& origin = nodeJs.at("origin");
            node.vOrigin = {{.x = origin.at(0).get<float>(), .y = origin.at(1).get<float>(), .z = origin.at(2).get<float>()}};
            node.fAngle = nodeJs.at("angle").get<float>();
            node.forward.x = nodeJs.at("forward").at(0).get<float>();
            node.forward.y = nodeJs.at("forward").at(1).get<float>();
            node.fRadius = nodeJs.at("radius").get<float>();
            node.minUseDistSq = nodeJs.at("minUseDistSq").get<float>();
            node.wOverlapNode[0] = nodeJs.at("overlap").at(0).get<int16_t>();
            node.wOverlapNode[1] = nodeJs.at("overlap").at(1).get<int16_t>();

            const auto& linksJs = nodeJs.at("links");
            node.totalLinkCount = static_cast<uint16_t>(linksJs.size());
            node.Links = linksJs.empty() ? nullptr : m_memory.Alloc<pathlink_s>(linksJs.size());
            for (size_t l = 0; l < linksJs.size(); l++)
            {
                const auto& linkJs = linksJs[l];
                auto& link = node.Links[l];
                link.nodeNum = linkJs.at("node").get<uint16_t>();
                if (link.nodeNum >= nodeCount)
                {
                    con::error("path node {} links to node {} of {}", i, link.nodeNum, nodeCount);
                    return false;
                }
                link.fDist = linkJs.at("dist").get<float>();
                link.disconnectCount = static_cast<char>(linkJs.at("disconnectCount").get<int>());
                link.negotiationLink = static_cast<char>(linkJs.at("negotiationLink").get<int>());
                link.flags = static_cast<char>(linkJs.at("flags").get<int>());
            }
            linkCount += linksJs.size();
        }

        const auto visHex = js.at("pathVis").get<std::string>();
        const auto visBits = static_cast<size_t>(nodeCount) * (nodeCount ? nodeCount - 1 : 0);
        const auto floorVisBytes = visBits / 8;
        const auto ceilVisBytes = (visBits + 7) / 8;
        const auto expectedVisBytes = visHex.size() / 2;
        if (visHex.size() % 2 || (expectedVisBytes != floorVisBytes && expectedVisBytes != ceilVisBytes))
        {
            con::error("pathVis has {} bytes, expected {} or {}", expectedVisBytes, floorVisBytes, ceilVisBytes);
            return false;
        }
        path.visBytes = static_cast<int>(expectedVisBytes);
        path.pathVis = expectedVisBytes ? m_memory.Alloc<char>(expectedVisBytes) : nullptr;
        for (size_t b = 0; b < expectedVisBytes; b++)
            path.pathVis[b] = ByteFromHex(visHex[2 * b], visHex[2 * b + 1]);

        path.smoothBytes = 0;
        path.smoothCache = nullptr;
        BuildNodeTree(path);

        con::info("Path nodes: {} nodes, {} links, {} vis bytes, {} tree entries", nodeCount, linkCount, path.visBytes, path.nodeTreeCount);
        return true;
    }

    bool GameWorldMpLinker::LinkGameWorldMp(const BSPData& bsp)
    {
        auto* gameWorldMp = m_memory.Alloc<GameWorldMp>();
        gameWorldMp->name = m_memory.Dup(bsp.bspName.c_str());

        auto& path = gameWorldMp->path;
        path.nodeCount = 0;
        path.originalNodeCount = 0;
        path.visBytes = 0;
        path.smoothBytes = 0;
        path.nodeTreeCount = 0;
        path.pathVis = nullptr;
        path.smoothCache = nullptr;
        path.nodeTree = nullptr;

        if (!LoadPaths(path))
            return false;
        if (!path.nodes)
        {
            // The game has 128 empty nodes allocated
            path.nodes = m_memory.Alloc<pathnode_t>(EXTRA_NODE_COUNT);
            path.basenodes = m_memory.Alloc<pathbasenode_t>(EXTRA_NODE_COUNT);
        }

        AssetRegistration<AssetGameWorldMp> registration(gameWorldMp->name, gameWorldMp);
        for (const auto scrString : m_script_strings)
            registration.AddScriptString(scrString);
        return m_context.AddAsset(std::move(registration)) != nullptr;
    }
} // namespace BSP
