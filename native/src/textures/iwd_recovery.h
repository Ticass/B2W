#pragma once

#include "core/zip.h"
#include "textures/wavelet.h"

#include <optional>
#include <span>
#include <unordered_map>

namespace w2b::textures
{
    struct RecoveredImage
    {
        std::filesystem::path path;
        std::filesystem::path archive;
        std::string entry;
        unsigned iwiFormat = 0;
        std::optional<std::filesystem::path> decoderTables;
    };

    class IwdRecovery
    {
    public:
        IwdRecovery(std::span<const std::filesystem::path> directories, std::filesystem::path root,
                    std::optional<std::filesystem::path> binary,
                    std::span<const std::filesystem::path> sourceImageRoots,
                    std::span<const std::filesystem::path> overrides);

        RecoveredImage Recover(const std::string& stem);
        RecoveredImage Resolve(const std::string& stem, const std::filesystem::path& source);

    private:
        struct Archive
        {
            std::filesystem::path path;
            bool overrides;
            std::unique_ptr<zip::Archive> contents;
        };

        struct Source { std::size_t archive, entry; };
        void AddArchive(const std::filesystem::path& path, bool overrides);
        bool Shadowed(std::span<const Source> copies, const std::filesystem::path& source);
        std::string Read(Source source);

        std::filesystem::path root_;
        std::optional<std::filesystem::path> binary_;
        std::vector<std::filesystem::path> sourceImageRoots_;
        std::vector<Archive> archives_;
        std::unordered_map<std::string, std::vector<Source>> entries_;
        std::unordered_map<std::string, RecoveredImage> recovered_;
        std::optional<WaveletTables> tables_;
    };
}
