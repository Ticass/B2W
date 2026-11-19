#pragma once

#include "sound/alias_row.h"

#include <filesystem>

namespace w2b::sound
{
    void WriteAliasCsv(const std::filesystem::path& destination, const std::vector<T6AliasRow>& rows);
}
