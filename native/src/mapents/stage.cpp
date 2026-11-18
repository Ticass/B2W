#include "mapents/entities.h"

#include "core/encoding.h"
#include "core/files.h"
#include "gsc/port/zone_scripts.h"
#include "world/collision_world.h"

#include <optional>

namespace w2b::mapents
{
    Json ConvertRequest(const Json& request, const world::CollisionWorld* collision)
    {
        Json graph;
        if (const auto script = request.find("zoneScript"); script != request.end())
            graph = gsc::port::ReadZoneGraphFile(files::FromUtf8(script->get_ref<const std::string&>())).ToJson();
        auto entries = Json::array();
        if (const auto source = request.find("source"); source != request.end())
        {
            const auto file = files::FromUtf8(source->get_ref<const std::string&>());
            std::string bytes;
            if (!files::ReadBytes(file, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(file));
            entries = Parse(encoding::UniversalNewlines(encoding::Utf8Replace(bytes)));
        }
        else
            entries = request.at("entities");
        try
        {
            auto output = Convert(std::move(entries), request.at("project").get_ref<const std::string&>(),
                                  request.at("animscripts"), graph.is_null() ? request.at("zoneNames") : graph.at("names"),
                                  request.at("respawnExclusion").get_ref<const std::string&>());
            const auto destination = request.find("bsp");
            if (destination == request.end())
                return output;
            std::optional<world::CollisionWorld> clip;
            if (!collision)
                if (const auto source = request.find("clip"); source != request.end())
                {
                    clip.emplace(world::ReadCollisionWorld(files::FromUtf8(source->get_ref<const std::string&>())));
                    collision = &*clip;
                }
            auto written = Write(std::move(output), files::FromUtf8(destination->get_ref<const std::string&>()),
                                 collision, graph.is_null() ? request.at("startZones") : graph.at("initial"));
            if (!graph.is_null())
                written["zoneGraph"] = std::move(graph);
            return written;
        }
        catch (const std::invalid_argument& error)
        {
            return {{"invalid", error.what()}};
        }
    }

    Json Run(const Path& path)
    {
        return ConvertRequest(Json::parse(files::ReadText(path)), nullptr);
    }
}
