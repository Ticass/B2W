#pragma once

#include "core/text.h"

#include <filesystem>

namespace w2b::sound
{
    util::Json BindPcm(const std::filesystem::path& project, const std::filesystem::path& pcmRoot);
    util::Json BindCurves(const std::filesystem::path& project, const std::filesystem::path& driver, bool approximate);
}
