#include "audio/flac_batch.h"

#include "core/files.h"

#include <windows.h>

#include <array>
#include <cstdlib>

namespace w2b::audio
{
    namespace
    {
        using files::fs::path;

        path FindBundledFfmpeg(const path& folder)
        {
            for (const auto& executable : files::GlobSuffix(folder / "imageio_ffmpeg/binaries", ".exe"))
                if (files::Utf8(executable.filename()).starts_with("ffmpeg-"))
                    return executable;
            return {};
        }
    }

    path FindFfmpeg(const path& explicitEncoder)
    {
        if (!explicitEncoder.empty())
            return explicitEncoder;
        if (const auto* env = _wgetenv(L"IMAGEIO_FFMPEG_EXE"); env && *env)
            return env;
        std::array<wchar_t, 32768> pathBuffer{};
        const auto length = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(pathBuffer.size()), pathBuffer.data(), nullptr);
        if (length > 0 && length < pathBuffer.size())
            return pathBuffer.data();
        for (const auto variable : {L"VIRTUAL_ENV", L"CONDA_PREFIX"})
            if (const auto* prefix = _wgetenv(variable); prefix && *prefix)
                if (auto bundled = FindBundledFfmpeg(path(prefix) / "Lib/site-packages"); !bundled.empty())
                    return bundled;
        if (const auto* resources = _wgetenv(L"W2B_RESOURCE_ROOT"); resources && *resources)
            for (const auto& folder : {path(resources), path(resources) / "_internal"})
                if (auto bundled = FindBundledFfmpeg(folder); !bundled.empty())
                    return bundled;
        const auto written = GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
        if (written == 0 || written >= pathBuffer.size())
            return {};
        for (auto folder = path(pathBuffer.data()).parent_path(); !folder.empty(); folder = folder.parent_path())
        {
            // the converter's own native tools folder (tools/bin/ffmpeg.exe)
            if (const auto shipped = folder / "tools/bin/ffmpeg.exe"; GetFileAttributesW(shipped.c_str()) != INVALID_FILE_ATTRIBUTES)
                return shipped;
            for (const auto& root : {folder, folder / "_internal"})
                if (auto bundled = FindBundledFfmpeg(root); !bundled.empty())
                    return bundled;
            if (folder == folder.root_path())
                break;
        }
        return {};
    }
}
