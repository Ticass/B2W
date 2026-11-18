#pragma once

#include "launcher/settings.h"

namespace w2b::pipeline
{
    bool HasMenuArtwork(const launcher::Settings& settings);
    void StageMenuSettings(const launcher::Settings& settings, const std::filesystem::path& root,
                           const std::filesystem::path& decoder = {});
    py::Json StageMenuSettingsRequest(const std::filesystem::path& request);
}
