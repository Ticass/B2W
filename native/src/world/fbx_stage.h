#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <optional>

namespace w2b::world
{
    py::Json StageWorldFbx(const std::filesystem::path& request);
    py::Json ExportWorldFbx(const std::filesystem::path& source, const std::filesystem::path& output,
                           std::optional<std::size_t> surfaceLimit);
}
