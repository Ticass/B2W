#pragma once

#include "convert.h"

#include <filesystem>

namespace w2b::audio
{
    // audio.stage(project, output, decoder, xwma_decoder): decode every
    // staged sound of project/sounds.stage.json to canonical 48 kHz PCM under
    // output, and write output/audio.stage.json. decoderText is str(decoder)
    // for the report ("" when there is none).
    py::Json Stage(const std::filesystem::path& project, const std::filesystem::path& output, const std::string& decoderText, const Tools& tools);
} // namespace w2b::audio
