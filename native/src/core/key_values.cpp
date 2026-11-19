#include "core/key_values.h"

#include <algorithm>
#include <stdexcept>

namespace w2b
{
    const std::string* KeyValues::Find(std::string_view key) const
    {
        const auto found = std::ranges::find_if(fields, [&](const Field& field) { return field.first == key; });
        return found == fields.end() ? nullptr : &found->second;
    }

    std::string* KeyValues::Find(std::string_view key)
    {
        const auto found = std::ranges::find_if(fields, [&](const Field& field) { return field.first == key; });
        return found == fields.end() ? nullptr : &found->second;
    }

    void KeyValues::Set(std::string_view key, std::string value)
    {
        (*this)[key] = std::move(value);
    }

    void KeyValues::Erase(std::string_view key)
    {
        std::erase_if(fields, [&](const Field& field) { return field.first == key; });
    }

    std::string& KeyValues::operator[](std::string_view key)
    {
        if (auto* value = Find(key))
            return *value;
        return fields.emplace_back(std::string(key), std::string()).second;
    }

    py::Json ToJson(const KeyValues& values)
    {
        auto out = py::Json::object();
        for (const auto& [key, value] : values)
            out[key] = value;
        return out;
    }

    KeyValues KeyValuesFromJson(const py::Json& object)
    {
        KeyValues out;
        out.fields.reserve(object.size());
        for (const auto& [key, value] : object.items())
        {
            if (!value.is_string())
                throw std::invalid_argument("field " + key + " is not text");
            out.fields.emplace_back(key, value.get<std::string>());
        }
        return out;
    }
} // namespace w2b
