#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <vector>

namespace w2b::world
{
    struct RenderWorld;

    struct ProbeRequest
    {
        std::filesystem::path source, project; // the T4 .reflectionprobes.json dump, the project root
        std::vector<std::filesystem::path> imageRoots;
        bool compensated = true;
    };

    // the map's reflection probes for the world's surfaces and models; returns the probe report
    py::Json StageProbes(const ProbeRequest& request, const RenderWorld& world);
}
