#pragma once

#include "launcher/images.h"
#include "launcher/settings.h"

#include <optional>

namespace w2b::pipeline
{
    struct MenuArtwork
    {
        launcher::Image blit, large, blur;
        bool aligned;
        std::string warning;
    };

    // menuart.material: a native T6 2D material (straight alpha, clamped, no mips)
    py::Json MenuMaterial(const std::string& image);
    std::optional<MenuArtwork> LoadMenuArtwork(const launcher::Settings& settings, const std::filesystem::path& decoder);
    void StageMenuArtwork(const launcher::Settings& settings, const std::filesystem::path& root,
                          const std::filesystem::path& compat, const std::filesystem::path& decoder,
                          MenuArtwork artwork, py::Json& metadata);
}
