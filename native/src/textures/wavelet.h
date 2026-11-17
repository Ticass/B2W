#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace w2b::textures
{
    struct WaveletCode
    {
        std::int16_t value;
        std::int16_t bits;
        bool operator==(const WaveletCode&) const = default;
    };

    using WaveletBook = std::array<WaveletCode, 4096>;
    using WaveletTables = std::array<WaveletBook, 3>;

    WaveletTables LoadWaveletTables(const std::filesystem::path& binary);
    std::string WaveletToDds(std::string_view blob, const WaveletTables& tables);
}
