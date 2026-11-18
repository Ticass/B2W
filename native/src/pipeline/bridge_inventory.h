#pragma once

#include "core/pytext.h"
#include <filesystem>
#include <optional>

namespace w2b::pipeline
{
    py::Json BridgeInventory(const std::vector<std::filesystem::path>& roots, const std::vector<std::filesystem::path>& mapRoots,
                             const std::optional<std::filesystem::path>& entities, const py::Json& levelWeapons, py::Json& warnings);
}
