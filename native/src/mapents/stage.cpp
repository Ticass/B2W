#include "mapents/entities.h"

#include "core/encoding.h"
#include "core/files.h"
#include "gsc/port/zone_scripts.h"
#include "world/collision_world.h"

#include <optional>

namespace w2b::mapents
{
    MapEntityResult ConvertMapEntities(const MapEntityRequest& request)
    {
        Json graph;
        if (request.zoneScript)
            graph = gsc::port::ReadZoneGraphFile(*request.zoneScript).ToJson();
        std::string bytes;
        if (!files::ReadBytes(request.source, bytes))
            throw std::runtime_error("cannot read " + files::Utf8(request.source));
        auto entries = Parse(encoding::UniversalNewlines(encoding::Utf8Replace(bytes)));
        auto output = Convert(std::move(entries), request.project, request.animscripts, graph.is_null() ? Json::array() : graph.at("names"),
                              request.respawnExclusion);
        auto written = Write(std::move(output), request.bsp, request.clip, graph.is_null() ? Json() : graph.at("initial"));
        written.zoneGraph = std::move(graph);
        return written;
    }
}
