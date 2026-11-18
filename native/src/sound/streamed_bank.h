#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::sound
{
    bool StreamFlacEnabled();
    py::Json PackStreamedAudio(std::vector<py::Json>& rows, const std::filesystem::path& pcmRoot,
                               const std::filesystem::path& encoder);
}
