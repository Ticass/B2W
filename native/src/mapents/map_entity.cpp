#include "mapents/map_entity.h"

#include "core/files.h"

#include <stdexcept>

namespace w2b::mapents
{
    py::Json ToJson(const MapEntities& entities)
    {
        auto out = py::Json::array();
        for (const auto& entity : entities)
            out.push_back(w2b::ToJson(entity));
        return out;
    }

    MapEntity MapEntityFromJson(const py::Json& entity)
    {
        try
        {
            return KeyValuesFromJson(entity);
        }
        catch (const std::invalid_argument& error)
        {
            throw std::invalid_argument("map entity " + std::string(error.what()));
        }
    }

    MapEntities ReadMapEntities(const std::filesystem::path& path)
    {
        const auto data = py::Json::parse(files::ReadText(path));
        MapEntities out;
        const auto& entities = data.at("entities");
        out.reserve(entities.size());
        for (const auto& entity : entities)
            out.push_back(MapEntityFromJson(entity));
        return out;
    }

    void WriteMapEntities(const std::filesystem::path& path, const MapEntities& entities)
    {
        files::WriteText(path, py::JsonDumps({{"entities", ToJson(entities)}}, 1) + "\n");
    }
} // namespace w2b::mapents
