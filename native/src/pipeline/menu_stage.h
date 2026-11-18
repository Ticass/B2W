#pragma once

#include "launcher/settings.h"

namespace w2b::pipeline
{
    std::string StageMenuSettings(const launcher::Settings& settings, const std::filesystem::path& root,
                                  const std::filesystem::path& decoder = {},
                                  const std::filesystem::path& compat = {});
    py::Json StageMenuSettingsRequest(const std::filesystem::path& request);
}
