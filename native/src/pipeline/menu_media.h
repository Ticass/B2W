#pragma once

#include <filesystem>

namespace w2b::pipeline
{
    class TemporaryMenuFile
    {
    public:
        TemporaryMenuFile();
        TemporaryMenuFile(const TemporaryMenuFile&) = delete;
        TemporaryMenuFile& operator=(const TemporaryMenuFile&) = delete;
        ~TemporaryMenuFile();
        const std::filesystem::path& Path() const { return path_; }

    private:
        std::filesystem::path path_;
    };
}
