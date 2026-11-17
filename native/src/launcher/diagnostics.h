#pragma once
// Port of diagnostics.create_bundle: a local support ZIP with the failure
// context, never game assets or source archives.

#include "settings.h"

namespace w2b::launcher
{
    fs::path CreateBundle(const Settings& settings, const std::string& console, const std::string& error = "");
    // the converter version shown in bundles and the About box
    const char* Version();
} // namespace w2b::launcher
