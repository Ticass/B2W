#pragma once
// One map entity: its key/value pairs in the order the map lists them, as in
// the WaW .ents dump and the project's BSP/entities.json.

#include "core/key_values.h"
#include "core/pytext.h"

#include <filesystem>
#include <vector>

namespace w2b::mapents
{
    using MapEntity = KeyValues;
    using MapEntities = std::vector<MapEntity>;

    py::Json ToJson(const MapEntities& entities);
    // throws std::invalid_argument on a value that is not text
    MapEntity MapEntityFromJson(const py::Json& entity);

    // the "entities" list of BSP/entities.json
    MapEntities ReadMapEntities(const std::filesystem::path& path);
    void WriteMapEntities(const std::filesystem::path& path, const MapEntities& entities);
} // namespace w2b::mapents
