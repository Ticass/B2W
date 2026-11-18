#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world
{
    struct RenderWorld;

    py::Json StageProbes(const py::Json& request, const RenderWorld* world);
    py::Json StageProbesRequest(const std::filesystem::path& request);
}
