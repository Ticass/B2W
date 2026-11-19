#include "sound/bank_csv.h"

#include "core/files.h"
#include "sound/bank_schema.h"

namespace w2b::sound
{
    namespace
    {
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
    }

    void WriteAliasCsv(const std::filesystem::path& destination, const std::vector<T6AliasRow>& rows)
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
        for (const auto& row : rows)
        {
            for (std::size_t i = 0; i < row.columns.size(); ++i)
            {
                if (i != 0)
                    csv += ',';
                AppendField(csv, row.columns[i]);
            }
            csv += "\r\n";
        }
        files::WriteBytes(destination, csv);
    }
}
