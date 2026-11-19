#include "mapents/map_entity.h"

#include "core/files.h"

#include <algorithm>
#include <stdexcept>

namespace w2b::mapents
{
    const std::string* MapEntity::Find(std::string_view key) const
    {
        const auto found = std::ranges::find_if(fields, [&](const auto& field) { return field.first == key; });
        return found == fields.end() ? nullptr : &found->second;
    }

    void MapEntity::Set(std::string_view key, std::string value)
    {
        const auto found = std::ranges::find_if(fields, [&](const auto& field) { return field.first == key; });
        if (found != fields.end())
            found->second = std::move(value);
        else
            fields.emplace_back(std::string(key), std::move(value));
    }

    void MapEntity::Erase(std::string_view key)
    {
        std::erase_if(fields, [&](const auto& field) { return field.first == key; });
    }

    py::Json ToJson(const MapEntity& entity)
    {
        auto out = py::Json::object();
        for (const auto& [key, value] : entity.fields)
            out[key] = value;
        return out;
    }

    py::Json ToJson(const MapEntities& entities)
    {
        auto out = py::Json::array();
        for (const auto& entity : entities)
            out.push_back(ToJson(entity));
        return out;
    }

    MapEntity MapEntityFromJson(const py::Json& entity)
    {
        MapEntity out;
        out.fields.reserve(entity.size());
        for (const auto& [key, value] : entity.items())
        {
            if (!value.is_string())
                throw std::invalid_argument("map entity field " + key + " is not text");
            out.fields.emplace_back(key, value.get<std::string>());
        }
        return out;
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
