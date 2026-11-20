#pragma once

#include "core/text.h"
#include "launcher/settings.h"

namespace w2b::pipeline
{
    void ValidateMenuSong(const launcher::Settings& settings);
    util::Json StageMenuSong(const launcher::Settings& settings, const std::filesystem::path& root,
                          const std::filesystem::path& decoder = {});
}
