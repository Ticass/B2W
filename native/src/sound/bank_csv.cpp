#include "sound/bank_csv.h"

#include "core/files.h"
#include "sound/bank_schema.h"

#include <unordered_map>

namespace w2b::sound
{
    namespace
    {
        using py::Json;

        void AppendField(std::string& output, std::string_view field)
        {
            if (field.find_first_of(",\"\r\n") == field.npos)
            {
                output += field;
                return;
            }
            output += '"';
            for (const char c : field)
            {
                if (c == '"')
                    output += '"';
                output += c;
            }
            output += '"';
        }

        void AppendValue(std::string& output, const Json& value)
        {
            if (value.is_string())
                AppendField(output, value.get_ref<const std::string&>());
            else if (value.is_boolean())
                output += value.get<bool>() ? "True" : "False";
            else if (value.is_number_integer())
                output += std::to_string(value.get<long long>());
            else if (!value.is_null())
                output += py::FloatRepr(value.get<double>());
        }

        const auto& ColumnIndices()
        {
            static const auto columns = []
            {
                std::unordered_map<std::string_view, std::size_t> result;
                result.reserve(T6AliasColumns.size());
                for (std::size_t i = 0; i < T6AliasColumns.size(); ++i)
                    result.emplace(T6AliasColumns[i], i);
                return result;
            }();
            return columns;
        }
    }

    void WriteAliasCsv(const std::filesystem::path& destination, const std::vector<Json>& rows)
    {
        std::string csv;
        csv.reserve(512 + rows.size() * 256);
        for (std::size_t i = 0; i < T6AliasColumns.size(); ++i)
        {
            if (i != 0)
                csv += ',';
            AppendField(csv, T6AliasColumns[i]);
        }
        csv += "\r\n";
        const auto& columns = ColumnIndices();
        for (const auto& row : rows)
        {
            std::array<const Json*, T6AliasColumns.size()> fields{};
            for (const auto& [key, value] : row.items())
                if (const auto column = columns.find(key); column != columns.end())
                    fields[column->second] = &value;
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                if (i != 0)
                    csv += ',';
                if (fields[i])
                    AppendValue(csv, *fields[i]);
            }
            csv += "\r\n";
        }
        files::WriteBytes(destination, csv);
    }
}
