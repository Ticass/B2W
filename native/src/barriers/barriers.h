#pragma once

#include "core/pytext.h"

#include <array>
#include <filesystem>
#include <span>

namespace w2b::world { struct CollisionWorld; }

namespace w2b::barriers
{
    using py::Json;
    using Path = std::filesystem::path;
    using Vec3 = std::array<double, 3>;

    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    std::string ReadBytes(const Path& file);
    double Sum(std::span<const double> values);
    Vec3 RepairedTranslation(const Path& file);
    Json RepairTiming(std::string_view script);
    std::string RetimeRepair(std::string_view raw, const Json& timing);
    Json StageRepairs(std::string_view asset, const Path& bo2, const Json& timing, const Path& destination);
    Json AlignBoards(Json& entities, const Path& bo2, const world::CollisionWorld* clip);
    Json DropChunks(Json& entities);
    Json Run(const Path& request);
}
