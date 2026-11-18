#pragma once

#include "launcher/images.h"

namespace w2b::pipeline::menu
{
    launcher::Image Resize(const launcher::Image& image, int width, int height);
    launcher::Image Crop(const launcher::Image& image, int x, int y, int width, int height);
    void Composite(launcher::Image& destination, const launcher::Image& source, int x = 0, int y = 0);
    std::vector<std::uint8_t> ResizeAlphaBox(const launcher::Image& image, int width, int height);
}
