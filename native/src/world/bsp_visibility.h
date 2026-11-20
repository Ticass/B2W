#pragma once

#include "core/text.h"
#include "world/render_world.h"

#include <span>

namespace w2b::world
{
    struct Visibility
    {
        util::Json data;
        util::Json summary;
        std::string warning;
    };

    Visibility AssignVisibility(RenderWorld& world, const std::filesystem::path& source,
                                const std::vector<std::vector<int>>& probes);
    void WriteVisibility(Visibility& visibility, std::span<const int> meshCells, const std::filesystem::path& path);
}
