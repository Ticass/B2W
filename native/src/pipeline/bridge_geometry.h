#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    py::Json StageBridgeGeometry(const py::Json& request);
    py::Json StageBridgeGeometryRequest(const std::filesystem::path& request);
}
