#pragma once

#include "core/text.h"

#include <filesystem>

namespace w2b::audio
{
    std::filesystem::path FindFfmpeg(const std::filesystem::path& explicitEncoder = {});
    util::Json EncodeFlacBatch(const std::string& encoder, const util::Json& tasks, unsigned workers);
}
