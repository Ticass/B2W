#include "cache.h"

#include "files.h"
#include "hash.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>

namespace w2b::cache
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string Env(const char* name)
        {
            char* value = nullptr;
            size_t size = 0;
            if (_dupenv_s(&value, &size, name) != 0 || !value)
                return {};
            std::string text(value);
            std::free(value);
            return text;
        }
    } // namespace

    bool Enabled()
    {
        return Env("WAW2BO2_BUILD_CACHE") != "0";
    }

    fs::path Root()
    {
        const auto overrideDir = Env("WAW2BO2_BUILD_CACHE_DIR");
        if (!overrideDir.empty())
            return files::FromUtf8(overrideDir);
        auto base = Env("LOCALAPPDATA");
        return files::FromUtf8(base) / "WawConverter" / "build_cache";
    }

    std::string FileIdentity(const fs::path& path)
    {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error)
            return files::Utf8(path) + ":missing";
        const auto time = fs::last_write_time(path, error);
        return files::Utf8(fs::absolute(path)) + ":" + std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
    }

    std::string Key(const std::initializer_list<std::string_view> parts)
    {
        std::string material = "format1";
        for (const auto part : parts)
        {
            material += std::to_string(part.size());
            material += ':';
            material += part;
        }
        return hash::Sha256Hex(material);
    }

    namespace
    {
        fs::path EntryPath(const std::string_view space, const std::string& key)
        {
            return Root() / std::string(space) / key.substr(0, 2) / (key + ".bin");
        }
    } // namespace

    std::optional<std::string> Load(const std::string_view space, const std::string& key)
    {
        if (!Enabled())
            return std::nullopt;
        std::string data;
        if (!files::ReadBytes(EntryPath(space, key), data) || data.size() < 64)
            return std::nullopt;
        std::string value = data.substr(64);
        if (hash::Sha256Hex(value) != std::string_view(data).substr(0, 64))
            return std::nullopt; // torn or damaged: compute again
        return value;
    }

    void Store(const std::string_view space, const std::string& key, const std::string_view value)
    {
        if (!Enabled())
            return;
        static std::atomic<unsigned> counter{0};
        try
        {
            const auto target = EntryPath(space, key);
            fs::create_directories(target.parent_path());
            auto partial = target;
            partial += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(counter++) + ".tmp";
            {
                std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
                const auto digest = hash::Sha256Hex(value);
                stream.write(digest.data(), static_cast<std::streamsize>(digest.size()));
                stream.write(value.data(), static_cast<std::streamsize>(value.size()));
            }
            std::error_code error;
            fs::rename(partial, target, error); // atomic publish; a concurrent writer of the same key wrote the same bytes
            if (error)
                fs::remove(partial, error);
        }
        catch (const std::exception&)
        {
            // caching never fails a build
        }
    }
} // namespace w2b::cache
