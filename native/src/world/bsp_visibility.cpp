#include "world/bsp_visibility.h"

#include "core/files.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        using util::Json;

        std::string Problem(const Json& visibility, std::size_t surfaceCount)
        {
            const auto cells = visibility.value("cells", Json{});
            if (cells.empty() || visibility.value("nodes", Json{}).empty())
                return "the dump holds no cells";
            if (cells.size() > 256)
                return std::to_string(cells.size()) + " cells, T6 walks at most 256";
            for (const auto& cell : cells)
                for (const auto& portal : cell.at("portals"))
                {
                    const auto target = portal.at("cell").get<int>();
                    if (target < 0 || static_cast<std::size_t>(target) >= cells.size())
                        return "a portal leads outside the cell list";
                }
            std::set<int> owners;
            for (const auto& cell : cells)
                for (const auto& surface : cell.at("surfaces"))
                    if (!owners.insert(surface.get<int>()).second)
                        return "a surface sits in two cells";
            if (!owners.empty() && *owners.rbegin() >= static_cast<int>(surfaceCount))
                return "a cell lists a surface outside the world";
            return {};
        }

        int CellAt(const Json& visibility, const std::array<double, 3>& point)
        {
            const auto& nodes = visibility.at("nodes");
            const auto& planes = visibility.at("planes");
            const auto count = visibility.at("cellCount").get<int>();
            std::int64_t index = 0;
            for (std::size_t depth = 0; depth < nodes.size(); ++depth)
            {
                const auto node = nodes.at(static_cast<std::size_t>(index)).get<int>();
                if (node <= count)
                    return std::max(node - 1, 0);
                const auto& plane = planes.at(node - count - 1);
                const auto distance = point[0] * plane.at(0).get<double>() + point[1] * plane.at(1).get<double>()
                    + point[2] * plane.at(2).get<double>() - plane.at(3).get<double>();
                index += distance > 0 ? 2 : nodes.at(static_cast<std::size_t>(index) + 1).get<int>();
            }
            throw std::runtime_error("cyclic visibility tree");
        }
    }

    Visibility AssignVisibility(RenderWorld& world, const std::filesystem::path& source,
                                const std::vector<std::vector<int>>& probes)
    {
        if (!files::fs::is_regular_file(source))
            return {{}, {}, "visibility: the render dump has no cells (re-extract gfxworld with the "
                            "visibility T4 dumper); the world links as one cell and walls hide nothing"};
        auto data = Json::parse(files::ReadText(source));
        if (const auto problem = Problem(data, world.surfaces.size()); !problem.empty())
            return {{}, {}, "visibility: " + problem + "; the world links as one cell and walls hide nothing"};

        auto& cells = data.at("cells");
        std::set<int> listed;
        std::size_t portals = 0;
        for (std::size_t i = 0; i < cells.size(); ++i)
        {
            auto& cell = cells[i];
            for (const auto& entry : cell.at("surfaces"))
            {
                const auto id = entry.get<int>();
                const auto index = id < 0 ? static_cast<std::int64_t>(world.surfaces.size()) + id : id;
                world.surfaces.at(static_cast<std::size_t>(index)).cell = static_cast<int>(i);
            }
            for (const auto& model : cell.at("smodels"))
                listed.insert(model.get<int>());
            portals += cell.at("portals").size();
            cell["probes"] = i < probes.size() ? Json(probes[i]) : Json::array();
        }
        std::size_t placed = 0;
        for (auto& surface : world.surfaces)
            if (surface.cell < 0 && !surface.brushModel)
            {
                std::array<double, 3> centre;
                for (std::size_t axis = 0; axis < centre.size(); ++axis)
                    centre[axis] = (static_cast<double>(surface.mins[axis]) + surface.maxs[axis]) * 0.5;
                surface.cell = CellAt(data, centre);
                ++placed;
            }
        for (std::size_t i = 0; i < world.staticModels.size(); ++i)
            if (!listed.contains(static_cast<int>(i)))
            {
                const auto& origin = world.staticModels[i].origin;
                cells.at(CellAt(data, {origin[0], origin[1], origin[2]})).at("smodels").push_back(i);
                ++placed;
            }
        Json summary{{"cells", cells.size()}, {"portals", portals}, {"nodes", data.at("nodes").size()},
                     {"placed_by_position", placed}};
        return {std::move(data), std::move(summary), {}};
    }

    void WriteVisibility(Visibility& visibility, std::span<const int> meshCells, const std::filesystem::path& path)
    {
        if (visibility.data.is_null())
            return;
        const auto& source = visibility.data;
        const auto& cells = source.at("cells");
        std::vector<std::vector<std::size_t>> meshes(cells.size());
        std::size_t visibleMeshes = 0;
        for (std::size_t i = 0; i < meshCells.size(); ++i)
            if (meshCells[i] >= 0)
            {
                meshes.at(meshCells[i]).push_back(i);
                ++visibleMeshes;
            }
        auto output = Json::array();
        for (std::size_t i = 0; i < cells.size(); ++i)
        {
            const auto& cell = cells[i];
            auto models = cell.at("smodels").get<std::vector<int>>();
            std::ranges::sort(models);
            output.push_back({{"mins", cell.at("mins")}, {"maxs", cell.at("maxs")}, {"meshes", std::move(meshes[i])},
                              {"smodels", std::move(models)}, {"probes", cell.at("probes")}, {"portals", cell.at("portals")}});
        }
        files::WriteText(path, util::JsonDumps({{"planes", source.at("planes")}, {"nodes", source.at("nodes")},
                                            {"cells", std::move(output)}}, -1) + "\n");
        visibility.summary["meshes"] = visibleMeshes;
    }
}
