#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::sound
{
    void WriteAliasCsv(const std::filesystem::path& destination, const std::vector<py::Json>& rows);
}
