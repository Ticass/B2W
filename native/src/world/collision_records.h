#pragma once
// The collision records the T6 linker reads from BSP/: brushes.json (world
// brushes), submodels.json (brush entities) and staticmodels.json.

#include "core/pytext.h"
#include "world/collision_world.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace w2b::world
{
    using RecordPoint = std::array<double, 3>;

    struct BrushSideRecord // a bevel plane beyond the six axial sides
    {
        RecordPoint normal{};
        double distance = 0;
        std::int32_t contents = 0, surfaceFlags = 0;
    };

    struct BrushRecord
    {
        RecordPoint mins{}, maxs{};
        std::int32_t contents = 0;
        std::vector<std::array<std::int32_t, 2>> axial; // contents, surface flags of the six axial sides
        std::vector<BrushSideRecord> sides;
        std::vector<RecordPoint> verts;
        py::Json ToJson() const;
        static BrushRecord FromJson(const py::Json& brush);
    };

    struct SubmodelRecord
    {
        RecordPoint mins{}, maxs{};
        std::vector<BrushRecord> brushes;
        py::Json ToJson() const;
    };
    using SubmodelRecords = std::vector<SubmodelRecord>;

    struct StaticModelRecord
    {
        std::string name;
        std::int32_t contents = 0;
        RecordPoint origin{};
        std::array<double, 9> invScaledAxis{};
        RecordPoint absmin{}, absmax{};
        py::Json ToJson() const;
    };

    struct BrushRecords
    {
        std::vector<BrushRecord> records;
        py::Json summary;
    };
    struct StaticModelRecords
    {
        std::vector<StaticModelRecord> records;
        py::Json summary;
    };

    BrushRecords BuildWorldBrushes(const CollisionWorld& world);
    SubmodelRecords BuildSubmodels(const CollisionWorld& world);
    StaticModelRecords BuildStaticModels(const CollisionWorld& world);

    // BSP/submodels.json
    SubmodelRecords ReadSubmodels(const std::filesystem::path& path);
    void WriteSubmodels(const std::filesystem::path& path, const SubmodelRecords& submodels);
}
