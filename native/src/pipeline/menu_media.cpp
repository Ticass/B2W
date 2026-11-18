#include "pipeline/menu_media.h"

#include "core/files.h"
#include "launcher/settings.h"

#include <windows.h>

#include <array>
#include <cstdlib>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    fs::path FindMenuDecoder(const fs::path& explicitDecoder)
    {
        if (!explicitDecoder.empty())
            return explicitDecoder;
        if (const auto* env = _wgetenv(L"IMAGEIO_FFMPEG_EXE"); env && *env)
            return env;
        std::array<wchar_t, 32768> found{};
        const auto length = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
        if (length > 0 && length < found.size())
            return found.data();
        const auto resources = launcher::ResourceRoot();
        for (const auto& root : {resources, resources / "_internal"})
        {
            const auto bundled = files::GlobSuffix(root / "imageio_ffmpeg/binaries", ".exe");
            for (const auto& file : bundled)
                if (files::Utf8(file.filename()).starts_with("ffmpeg-"))
                    return file;
        }
        return {};
    }

    TemporaryMenuFile::TemporaryMenuFile()
    {
        // GetTempFileName creates the file before returning its unique name.
        std::array<wchar_t, MAX_PATH> name{};
        if (!GetTempFileNameW(fs::temp_directory_path().c_str(), L"w2b", 0, name.data()))
            throw std::runtime_error("cannot allocate a temporary menu media file");
        path_ = name.data();
    }

    TemporaryMenuFile::~TemporaryMenuFile()
    {
        std::error_code error;
        fs::remove(path_, error);
    }
}
