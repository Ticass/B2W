#pragma once
// One row of the official BO2 alias CSV (soundbank/<bank>.aliases.csv): the
// sixty T6 columns as text, as the linker reads them, and the WaW falloff the
// row was mixed from (not written; the bank report and remaster read it).

#include "core/text.h"
#include "sound/bank_schema.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace w2b::sound
{
    constexpr std::size_t AliasColumn(std::string_view name)
    {
        for (std::size_t i = 0; i < T6AliasColumns.size(); ++i)
            if (T6AliasColumns[i] == name)
                return i;
        throw std::out_of_range("not a T6 alias column");
    }

    struct T6AliasRow
    {
        std::array<std::string, T6AliasColumns.size()> columns;
        util::Json waw; // the WaW falloff descriptor (standard, falloff, wet_ratio, ...)

        std::string& operator[](std::string_view column)
        {
            return columns[AliasColumn(column)];
        }
        const std::string& operator[](std::string_view column) const
        {
            return columns[AliasColumn(column)];
        }
    };
} // namespace w2b::sound
