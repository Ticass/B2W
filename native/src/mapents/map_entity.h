#pragma once
// One map entity: its key/value pairs in the order the map lists them, as in
// the WaW .ents dump and the project's BSP/entities.json.

#include "core/pytext.h"

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w2b::mapents
{
    struct MapEntity
    {
        std::vector<std::pair<std::string, std::string>> fields;

        MapEntity() = default;
        MapEntity(std::initializer_list<std::pair<std::string, std::string>> init) : fields(init) {}

        const std::string* Find(std::string_view key) const;
        std::string_view Get(std::string_view key, std::string_view fallback = {}) const
        {
            const auto* value = Find(key);
            return value ? std::string_view(*value) : fallback;
        }
        bool Has(std::string_view key) const { return Find(key) != nullptr; }
        // replaces the value in place, or appends the key
        void Set(std::string_view key, std::string value);
        void Erase(std::string_view key);
    };
    using MapEntities = std::vector<MapEntity>;

    py::Json ToJson(const MapEntity& entity);
    py::Json ToJson(const MapEntities& entities);
    // throws std::invalid_argument on a value that is not text
    MapEntity MapEntityFromJson(const py::Json& entity);

    // the "entities" list of BSP/entities.json
    MapEntities ReadMapEntities(const std::filesystem::path& path);
    void WriteMapEntities(const std::filesystem::path& path, const MapEntities& entities);
} // namespace w2b::mapents
