#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::materials
{
    py::Json IndexDonors(const std::filesystem::path& stockMaterials);
}
