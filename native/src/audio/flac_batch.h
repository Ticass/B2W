#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::audio
{
    py::Json EncodeFlacBatch(const std::filesystem::path& request);
}
