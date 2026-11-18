#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world
{
    std::filesystem::path WriteMapZone(const std::filesystem::path& stage, const std::string& project,
                                       bool ipak, const py::Json& assets);
    py::Json WriteMapZoneRequest(const std::filesystem::path& request);
}
