#include "audio/flac_batch.h"

#include <windows.h>

#include <array>
#include <cstdlib>

namespace w2b::audio
{
    using std::filesystem::path;

    // the encoder given, W2B_FFMPEG, ffmpeg.exe on PATH, else the copy the
    // converter ships in tools/bin beside (or above) this executable
    path FindFfmpeg(const path& explicitEncoder)
    {
        if (!explicitEncoder.empty())
            return explicitEncoder;
        if (const auto* env = _wgetenv(L"W2B_FFMPEG"); env && *env)
            return env;
        std::array<wchar_t, 32768> buffer{};
        const auto length = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length > 0 && length < buffer.size())
            return buffer.data();
        const auto written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0 || written >= buffer.size())
            return {};
        for (auto folder = path(buffer.data()).parent_path(); !folder.empty(); folder = folder.parent_path())
        {
            if (const auto shipped = folder / "tools/bin/ffmpeg.exe"; GetFileAttributesW(shipped.c_str()) != INVALID_FILE_ATTRIBUTES)
                return shipped;
            if (folder == folder.root_path())
                break;
        }
        return {};
    }
}
