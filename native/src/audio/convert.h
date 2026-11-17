#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::audio
{
    class SoundError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Chunks
    {
        std::string form;                                       // "WAVE" or "XWMA"
        std::vector<std::pair<std::string, std::string>> parts; // tag -> bytes, file order
        const std::string* Find(std::string_view tag) const;
    };

    // audio.chunks
    Chunks ParseChunks(const std::string& data);

    // audio.canonical_pcm
    std::string CanonicalPcm(std::string_view pcm, unsigned channels, unsigned rate);

    struct Tools
    {
        std::optional<std::filesystem::path> xwmaHelper; // xwma.find_helper result
        std::optional<std::filesystem::path> resampler;  // audio.find_decoder(None) result
    };

    // audio.convert(source): canonical WAV (nullopt for loop-region sounds) and its report
    std::pair<std::optional<std::string>, py::Json> Convert(const std::filesystem::path& source, const Tools& tools);
} // namespace w2b::audio
