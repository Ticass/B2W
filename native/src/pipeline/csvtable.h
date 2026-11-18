#pragma once
// Python's csv module with its default dialect: csv.reader over decoded text
// and csv.writer (QUOTE_MINIMAL) with a chosen line terminator.

#include <string>
#include <string_view>
#include <vector>

namespace w2b::pipeline
{
    using CsvRows = std::vector<std::vector<std::string>>;

    // list(csv.reader(text)): an empty line is an empty row
    CsvRows ReadCsv(std::string_view text);
    // csv.writer(stream, lineterminator=terminator).writerows(rows)
    std::string CsvText(const CsvRows& rows, std::string_view terminator);
} // namespace w2b::pipeline
