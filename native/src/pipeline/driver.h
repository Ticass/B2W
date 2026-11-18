#pragma once
// Port of builddriver.py and cli._build_map: the desktop build pipeline. Steps
// that are native run in this process; the ones still in Python run as CLI
// steps of the conversion worker (stage-art, stage-bridge, build-mod).

#include "launcher/build.h"

#include <optional>

namespace w2b::pipeline
{
    struct BuildOptions
    {
        bool redump = false;
        std::optional<std::filesystem::path> root; // --build-root
        std::optional<launcher::CachePaths> caches; // --waw-cache-root / --bo2-cache-root
    };

    // cli build-map: preflight, then the locked build; throws StepError
    int BuildMap(const launcher::Settings& settings, const BuildOptions& options);
} // namespace w2b::pipeline
