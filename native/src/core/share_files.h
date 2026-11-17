#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::files
{
    py::Json ShareIdenticalFiles(const std::filesystem::path& request);
}
