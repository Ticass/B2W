#include "barriers/barriers.h"

#include "core/files.h"
#include "world/collision_world.h"

#include <map>

namespace w2b::barriers
{
    namespace
    {
        Json UpdateBoards(const Path& bsp, const Path* bo2, const world::CollisionWorld* clip, bool drop)
        {
            const auto path = bsp / "entities.json";
            auto data = Json::parse(ReadBytes(path));
            auto& entities = data.at("entities");
            auto result = Json::object();
            if (bo2)
                result["alignment"] = AlignBoards(entities, *bo2, clip);
            if (drop)
                result["chunks"] = DropChunks(entities);
            if (bo2 || result.at("chunks").at("dropped_brushes") != 0)
                files::WriteText(path, py::JsonDumps(data, 1) + '\n');
            return result;
        }

        Path ReadPath(const Json& request, std::string_view key)
        {
            return files::FromUtf8(request.at(key).get_ref<const std::string&>());
        }

        Json StageBoardsRequest(const Json& request, bool align, bool drop)
        {
            std::optional<Path> bo2;
            std::optional<world::CollisionWorld> clip;
            if (align)
            {
                bo2 = ReadPath(request, "bo2");
                if (request.contains("clip"))
                    clip.emplace(world::ReadCollisionWorld(ReadPath(request, "clip")));
            }
            return UpdateBoards(ReadPath(request, "bsp"), bo2 ? &*bo2 : nullptr, clip ? &*clip : nullptr, drop);
        }

        Json FindTiming(const Json& request)
        {
            for (const auto& source : request.at("scripts"))
            {
                auto timing = RepairTiming(source.at(1).get_ref<const std::string&>());
                if (!timing.is_null())
                {
                    Json result{{"script", source.at(0)}};
                    result.update(timing);
                    return result;
                }
            }
            return nullptr;
        }
    }

    Json StageBoards(const Path& bsp, const Path& bo2, const world::CollisionWorld* clip)
    {
        return UpdateBoards(bsp, &bo2, clip, true);
    }

    Json Run(const Path& file)
    {
        using Operation = Json (*)(const Json&);
        static const std::map<std::string_view, Operation, std::less<>> operations{
            {"translation", [](const Json& request) -> Json { return RepairedTranslation(ReadPath(request, "source")); }},
            {"align", [](const Json& request) { return StageBoardsRequest(request, true, false).at("alignment"); }},
            {"drop", [](const Json& request) { return StageBoardsRequest(request, false, true).at("chunks"); }},
            {"boards", [](const Json& request) { return StageBoardsRequest(request, true, true); }},
            {"timing", FindTiming},
            {"repair", [](const Json& request) {
                files::WriteBytes(ReadPath(request, "destination"),
                    RetimeRepair(ReadBytes(ReadPath(request, "source")), request.at("timing")));
                return Json();
            }},
            {"stage-repairs", [](const Json& request) {
                return StageRepairs(request.at("asset").get_ref<const std::string&>(), ReadPath(request, "bo2"),
                                    request.at("timing"), ReadPath(request, "destination"));
            }},
        };
        try
        {
            const auto request = Json::parse(ReadBytes(file));
            const auto op = operations.find(request.at("operation").get_ref<const std::string&>());
            if (op == operations.end())
                throw Error("unknown barricade operation");
            return {{"result", op->second(request)}};
        }
        catch (const Error& error)
        {
            return {{"invalid", error.what()}};
        }
        catch (const std::invalid_argument& error)
        {
            return {{"invalid", error.what()}};
        }
    }
}
