#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::audio
{
    std::filesystem::path FindFfmpeg(const std::filesystem::path& explicitEncoder = {});
    py::Json EncodeFlacBatch(const std::string& encoder, const py::Json& tasks, unsigned workers);
}
