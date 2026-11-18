#pragma once

#include "textures/iwi.h"

#include <span>

namespace w2b::textures
{
    IwiImage EncodeMenuTexture(std::span<const std::uint8_t> rgba, unsigned width, unsigned height, Format format);
}
