#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::sound
{
    py::Json BindPcm(const std::filesystem::path& project, const std::filesystem::path& pcmRoot);
    py::Json BindCurves(const std::filesystem::path& project, const std::filesystem::path& driver, bool approximate);
}
