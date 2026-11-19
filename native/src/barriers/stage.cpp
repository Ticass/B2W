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
            auto entities = mapents::ReadMapEntities(path);
            auto result = Json::object();
            if (bo2)
                result["alignment"] = AlignBoards(entities, *bo2, clip);
            if (drop)
                result["chunks"] = DropChunks(entities);
            if (bo2 || result.at("chunks").at("dropped_brushes") != 0)
                mapents::WriteMapEntities(path, entities);
            return result;
        }
    }

    Json StageBoards(const Path& bsp, const Path& bo2, const world::CollisionWorld* clip)
    {
        return UpdateBoards(bsp, &bo2, clip, true);
    }
}
