#include "pipeline/menu_media.h"

#include <windows.h>

#include <array>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

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
