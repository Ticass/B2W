#pragma once

#include "core/encoding.h"

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace w2b::world
{
    class BspReader
    {
    public:
        explicit BspReader(const std::filesystem::path& path)
            : input_(path, std::ios::binary | std::ios::ate)
        {
            static_assert(std::endian::native == std::endian::little);
            if (!input_)
                throw std::runtime_error("could not open BSP interchange file");
            const auto size = input_.tellg();
            if (size < 0 || !input_.seekg(0))
                throw std::runtime_error("could not seek in BSP interchange file");
            remaining_ = static_cast<std::uint64_t>(size);
        }

        void Exact(void* destination, std::size_t bytes)
        {
            CheckCount(bytes, 1);
            if (bytes && !input_.read(static_cast<char*>(destination), bytes))
                throw std::runtime_error("truncated BSP interchange file");
            remaining_ -= bytes;
        }

        [[nodiscard]] std::uint64_t Remaining() const { return remaining_; }

        void CheckCount(std::size_t count, std::size_t recordSize) const
        {
            if (count > remaining_ / recordSize)
                throw std::runtime_error("truncated BSP interchange file");
        }

        template <typename T>
        T One()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            T value;
            Exact(&value, sizeof(value));
            return value;
        }

        template <typename T>
        std::vector<T> Array(std::size_t count)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            CheckCount(count, sizeof(T));
            std::vector<T> result(count);
            Exact(result.data(), count * sizeof(T));
            return result;
        }

        std::string String()
        {
            const auto length = One<std::uint32_t>();
            if (length > 1'048'576)
                throw std::runtime_error("unreasonable BSP interchange string length");
            CheckCount(length, 1);
            std::string value(length, '\0');
            Exact(value.data(), length);
            if (encoding::Utf8Replace(value) != value)
                throw std::runtime_error("invalid UTF-8 in BSP interchange name");
            return value;
        }

    private:
        std::ifstream input_;
        std::uint64_t remaining_;
    };
}
