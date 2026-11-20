#pragma once

#include "core/text.h"

#include <filesystem>

namespace w2b::materials
{
    util::Json IndexDonors(const std::filesystem::path& stockMaterials);
}
