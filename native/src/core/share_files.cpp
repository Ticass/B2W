#include "core/share_files.h"

#include "core/files.h"
#include "core/parallel.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <numeric>

namespace w2b::files
{
    namespace
    {
        bool EqualBytes(const fs::path& first, const fs::path& second, std::uintmax_t size)
        {
            std::ifstream a(first, std::ios::binary), b(second, std::ios::binary);
            std::array<char, 64 * 1024> left{}, right{};
            while (size)
            {
                const auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(size, left.size()));
                if (!a.read(left.data(), count) || !b.read(right.data(), count) ||
                    !std::equal(left.begin(), left.begin() + count, right.begin()))
                    return false;
                size -= static_cast<std::uintmax_t>(count);
            }
            return true;
        }

        bool Share(const fs::path& source, const fs::path& target, std::size_t id)
        {
            const auto link = target.parent_path() / ("w2b-share-" + std::to_string(id) + ".tmp");
            std::error_code error;
            fs::create_hard_link(source, link, error);
            if (error)
                return false;
            fs::rename(link, target, error);
            if (!error)
                return true;
            std::error_code cleanup;
            fs::remove(link, cleanup);
            if (cleanup)
                throw fs::filesystem_error("could not remove temporary cache link", link, cleanup);
            return false;
        }

        std::uintmax_t FindAndShare(const fs::path& output, const std::vector<fs::path>& prior, fs::path relative,
                                    std::size_t index)
        {
            if (relative.has_root_path() || std::ranges::any_of(relative, [](const auto& part) { return part == ".."; }))
                throw std::runtime_error("invalid relative cache asset path");
            const auto target = output / relative;
            std::error_code error;
            const auto size = fs::file_size(target, error);
            if (error || size < 256 * 1024)
                return 0;
            for (const auto& folder : prior)
            {
                const auto source = folder / relative;
                if (fs::file_size(source, error) != size || error || fs::equivalent(source, target, error))
                    continue;
                if (EqualBytes(source, target, size) && Share(source, target, index))
                    return size;
            }
            return 0;
        }
    }

    SharedFiles ShareIdenticalFiles(const fs::path& staging, const std::vector<fs::path>& prior, const std::vector<fs::path>& names)
    {
        std::vector<std::uintmax_t> sizes(names.size());
        parallel::For(names.size(), [&](std::size_t index)
        {
            sizes[index] = FindAndShare(staging, prior, names[index], index);
        }, 4);
        return {static_cast<std::size_t>(std::ranges::count_if(sizes, [](auto size) { return size != 0; })),
                std::accumulate(sizes.begin(), sizes.end(), std::uintmax_t{})};
    }
}
