#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::effects
{
    py::Json StageEffects(const std::filesystem::path& request);
    py::Json StageModelOverlayRuntime(const std::filesystem::path& project, const py::Json& effects);
    py::Json StageModelOverlayRuntimeRequest(const std::filesystem::path& request);
}
