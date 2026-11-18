#include "barriers/xanim.h"

#include "core/files.h"

#include <algorithm>

namespace w2b::barriers
{
    std::string ReadBytes(const Path& file)
    {
        std::string bytes;
        if (!files::ReadBytes(file, bytes))
            throw Error("cannot read " + files::Utf8(file));
        return bytes;
    }

    double Sum(std::span<const double> values)
    {
        // Match Python's compensated sum for floating-point geometry.
        double high = 0, low = 0;
        for (const auto value : values)
        {
            const auto next = high + value;
            low += std::abs(high) >= std::abs(value) ? (high - next) + value : (value - next) + high;
            high = next;
        }
        return low != 0 && std::isfinite(low) ? high + low : high;
    }

    Vec3 RepairedTranslation(const Path& file)
    {
        const auto bytes = ReadBytes(file);
        Reader input(bytes);
        const auto header = ReadHeader(input);
        const auto prefix = files::Utf8(file) + ": ";
        if (header.version != 19 || header.flags != 2 || !header.bones || !header.fps)
            throw Error(prefix + "unsupported barricade repair animation header");
        if (input.Read<std::uint16_t>() != 0)
            throw Error(prefix + "barricade repair has an unsupported delta rotation");
        const auto count = input.Read<std::uint16_t>();
        if (count == 1)
            return ReadVector(input);
        if (count == 0)
            throw Error(prefix + "barricade repair has no root translation");
        if (count < header.frames)
        {
            const bool smallIndex = header.frames <= 256;
            input.Take(static_cast<std::size_t>(count - 1) * (smallIndex ? 1 : 2));
            if (ReadIndex(input, smallIndex) != static_cast<unsigned>(header.frames - 1))
                throw Error(prefix + "barricade repair omits its final frame");
        }
        const auto small = input.Read<std::uint8_t>();
        if (small > 1)
            throw Error(prefix + "invalid compressed translation flag");
        const TransQuantization track{small != 0, ReadVector(input), ReadVector(input)};
        input.Take(static_cast<std::size_t>(count - 1) * 3 * (track.small ? 1 : 2));
        const auto pose = track.Key(input);
        if (!std::ranges::all_of(pose, [](auto value) { return std::isfinite(value); }))
            throw Error(prefix + "non-finite repaired translation");
        return pose;
    }
}
