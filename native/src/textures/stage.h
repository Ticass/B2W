#pragma once

#include "core/pytext.h"
#include "textures/iwi.h"

#include <filesystem>

namespace w2b::textures
{
    py::Json HeaderJson(const IwiHeader& header);
    IwiHeader ConvertFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                          bool faceMajor = false, bool baseOnly = false);
    py::Json StageLightmaps(const std::filesystem::path& request);
}
