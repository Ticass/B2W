#pragma once
// Text key/value pairs in their authored order: a map entity, a WaW
// infostring (weapon, flame table, physics preset). Lookups are linear, as
// the files list each key once and hold a few hundred at most.

#include "core/pytext.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w2b
{
    struct KeyValues
    {
        using Field = std::pair<std::string, std::string>;
        std::vector<Field> fields;

        KeyValues() = default;
        KeyValues(std::initializer_list<Field> init) : fields(init) {}

        const std::string* Find(std::string_view key) const;
        std::string* Find(std::string_view key);
        std::string_view Get(std::string_view key, std::string_view fallback = {}) const
        {
            const auto* value = Find(key);
            return value ? std::string_view(*value) : fallback;
        }
        bool Has(std::string_view key) const { return Find(key) != nullptr; }
        // replaces the value in place, or appends the key
        void Set(std::string_view key, std::string value);
        void Erase(std::string_view key);
        // the value of `key`, appended empty when missing
        std::string& operator[](std::string_view key);

        std::size_t size() const { return fields.size(); }
        bool empty() const { return fields.empty(); }
        auto begin() { return fields.begin(); }
        auto end() { return fields.end(); }
        auto begin() const { return fields.begin(); }
        auto end() const { return fields.end(); }
        bool operator==(const KeyValues&) const = default;
    };

    // no implicit JSON conversion (nlohmann would make an array of pairs): use ToJson
    void to_json(py::Json&, const KeyValues&) = delete;
    py::Json ToJson(const KeyValues& values);
    // throws std::invalid_argument on a value that is not text
    KeyValues KeyValuesFromJson(const py::Json& object);
} // namespace w2b
