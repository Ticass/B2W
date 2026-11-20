#pragma once

#include "core/text.h"
#include "sound/alias_row.h"

#include <filesystem>

namespace w2b::sound
{
    bool StreamFlacEnabled();
    util::Json PackStreamedAudio(std::vector<T6AliasRow>& rows, const std::filesystem::path& pcmRoot,
                               const std::filesystem::path& encoder);
}
