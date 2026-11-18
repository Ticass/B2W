#pragma once

#include "barriers/barriers.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace w2b::barriers
{
    class Reader
    {
    public:
        explicit Reader(std::string_view data) : rest_(data) {}

        std::string_view Take(std::size_t size)
        {
            if (size > rest_.size())
                throw Error("truncated barricade animation");
            const auto result = rest_.substr(0, size);
            rest_.remove_prefix(size);
            return result;
        }

        template<typename T>
        T Read()
        {
            static_assert(std::endian::native == std::endian::little);
            static_assert(std::is_trivially_copyable_v<T>);
            const auto bytes = Take(sizeof(T));
            T value;
            std::memcpy(&value, bytes.data(), sizeof(T));
            return value;
        }

        [[nodiscard]] std::string_view Remaining() const { return rest_; }

    private:
        std::string_view rest_;
    };

    struct XAnimHeader
    {
        std::uint16_t version, frames, bones;
        std::uint8_t flags, kind;
        std::uint16_t fps;
    };

    inline XAnimHeader ReadHeader(Reader& reader)
    {
        static_assert(sizeof(XAnimHeader) == 10);
        return reader.Read<XAnimHeader>();
    }

    inline Vec3 ReadVector(Reader& reader)
    {
        const auto value = reader.Read<std::array<float, 3>>();
        return {value[0], value[1], value[2]};
    }

    inline unsigned ReadIndex(Reader& reader, bool small)
    {
        return small ? reader.Read<std::uint8_t>() : reader.Read<std::uint16_t>();
    }

    struct TransQuantization
    {
        bool small;
        Vec3 mins, ranges;

        Vec3 Key(Reader& input) const
        {
            Vec3 key{};
            const auto maximum = small ? 255.0 : 65535.0;
            for (std::size_t i = 0; i < key.size(); ++i)
                key[i] = mins[i] + ranges[i] * ReadIndex(input, small) / maximum;
            return key;
        }
    };
}
