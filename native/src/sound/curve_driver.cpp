#include "sound/curves.h"

#include "core/files.h"
#include "sound/stage.h"

#include <cstdint>
#include <cstring>

namespace w2b::sound
{
    namespace
    {
        template<typename T> T Read(std::string_view data, std::size_t offset)
        {
            T value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }
    }

    std::vector<Curve> ReadDriverCurves(const std::filesystem::path& driver)
    {
        std::string bytes;
        const auto label = files::Utf8(std::filesystem::path(driver).make_preferred());
        if (!files::ReadBytes(driver, bytes))
            throw SoundError("cannot read " + label);
        if (!bytes.starts_with(std::string_view("W2BSDG1\0", 8)))
            throw SoundError(label + ": not a version 1 T6 sound driver sidecar");
        constexpr std::array strides{80u, 100u, 60u, 36u, 36u, 240u, 60u, 100u};
        std::string_view curveTable;
        std::size_t offset = 8;
        for (std::size_t i = 0; i < strides.size(); ++i)
        {
            if (bytes.size() - offset < 8)
                throw SoundError(label + ": truncated sound driver");
            const auto count = Read<std::uint32_t>(bytes, offset);
            const auto stride = Read<std::uint32_t>(bytes, offset + 4);
            offset += 8;
            if (stride != strides[i] || count > (bytes.size() - offset) / stride)
                throw SoundError(label + ": invalid sound driver table");
            const auto size = static_cast<std::size_t>(count) * stride;
            if (i == 1)
                curveTable = std::string_view(bytes).substr(offset, size);
            offset += size;
        }
        if (offset != bytes.size())
            throw SoundError(label + ": trailing sound driver data");
        const auto count = curveTable.size() / 100;
        if (count > 64)
            throw SoundError("T6 alias curve indices have six bits");
        std::vector<Curve> curves;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto row = curveTable.substr(i * 100, 100);
            const auto name = row.substr(0, 32);
            Curve curve{std::string(name.substr(0, name.find('\0'))), static_cast<int>(i), {}};
            for (std::size_t p = 36; p < 100; p += 8)
                curve.points.push_back({Read<float>(row, p), Read<float>(row, p + 4)});
            curves.push_back(std::move(curve));
        }
        return curves;
    }
}
