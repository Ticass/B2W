#include "mapents/entities.h"

#include "core/encoding.h"
#include "core/files.h"
#include "world/collision_world.h"

#include <optional>

namespace w2b::mapents
{
    Json Run(const Path& path)
    {
        const auto request = Json::parse(files::ReadText(path));
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
                                  request.at("animscripts"), request.at("zoneNames"),
                                  request.at("respawnExclusion").get_ref<const std::string&>());
            const auto destination = request.find("bsp");
            if (destination == request.end())
                return output;
            std::optional<world::CollisionWorld> clip;
            if (const auto source = request.find("clip"); source != request.end())
                clip.emplace(world::ReadCollisionWorld(files::FromUtf8(source->get_ref<const std::string&>())));
            return Write(std::move(output), files::FromUtf8(destination->get_ref<const std::string&>()),
                         clip ? &*clip : nullptr, request.at("startZones"));
        }
        catch (const std::invalid_argument& error)
        {
            return {{"invalid", error.what()}};
        }
    }
}
