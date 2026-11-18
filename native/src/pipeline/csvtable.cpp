// csv.reader / csv.writer (see csvtable.h).
#include "csvtable.h"

namespace w2b::pipeline
{
    CsvRows ReadCsv(const std::string_view text)
    {
        // the states of CPython's Modules/_csv.c parse_process_char, strict off
        enum State
        {
            StartRecord,
            StartField,
            InField,
            InQuoted,
            QuoteInQuoted,
        };
        CsvRows rows;
        std::vector<std::string> row;
        std::string field;
        State state = StartRecord;
        const auto endField = [&]
        {
            row.push_back(std::move(field));
            field.clear();
        };
        const auto endRow = [&]
        {
            rows.push_back(std::move(row));
            row.clear();
        };
        size_t i = 0;
        while (i < text.size())
        {
            // a physical line: up to and including its terminator
            size_t end = i;
            while (end < text.size() && text[end] != '\n' && text[end] != '\r')
                end++;
            size_t next = end;
            if (next < text.size())
                next += text[next] == '\r' && next + 1 < text.size() && text[next + 1] == '\n' ? 2 : 1;
            const std::string_view line = text.substr(i, end - i);
            const bool terminated = end < text.size();
            for (const char c : line)
            {
                switch (state)
                {
                case StartRecord:
                case StartField:
                    if (c == '"')
                        state = InQuoted;
                    else if (c == ',')
                        endField(), state = StartField;
                    else
                        field += c, state = InField;
                    break;
                case InField:
                    if (c == ',')
                        endField(), state = StartField;
                    else
                        field += c;
                    break;
                case InQuoted:
                    if (c == '"')
                        state = QuoteInQuoted;
                    else
                        field += c;
                    break;
                case QuoteInQuoted:
                    if (c == '"')
                        field += c, state = InQuoted;
                    else if (c == ',')
                        endField(), state = StartField;
                    else
                        field += c, state = InField;
                    break;
                }
            }
            // the line's end
            if (state == InQuoted)
            {
                // a quoted field spans lines and keeps its line break
                if (terminated)
                    field.append(text.substr(end, next - end));
            }
            else if (state == StartRecord)
                endRow(); // an empty line
            else
            {
                endField();
                endRow();
                state = StartRecord;
            }
            i = next;
        }
        if (state != StartRecord)
        {
            // end of input inside a record
            endField();
            endRow();
        }
        return rows;
    }

    std::string CsvText(const CsvRows& rows, const std::string_view terminator)
    {
        std::string text;
        for (const auto& row : rows)
        {
            for (size_t i = 0; i < row.size(); i++)
            {
                const auto& field = row[i];
                if (i)
                    text += ',';
                bool quote = field.find_first_of(",\"\r\n") != std::string::npos || field.find_first_of(terminator) != std::string::npos;
                // a lone empty field is written quoted, so the row is not an empty line
                if (field.empty() && row.size() == 1)
                    quote = true;
                if (quote)
                {
                    text += '"';
                    for (const char c : field)
                        text += c == '"' ? std::string("\"\"") : std::string(1, c);
                    text += '"';
                }
                else
                    text += field;
            }
            text += terminator;
        }
        return text;
    }
} // namespace w2b::pipeline
