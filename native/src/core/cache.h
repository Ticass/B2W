#pragma once
// Content-addressed store for pure steps, shared by every build (the native
// side of buildcache.py): an entry is keyed by everything its result depends
// on, so a changed input or tool is a different key, never a stale hit.
// WAW2BO2_BUILD_CACHE=0 turns it off; WAW2BO2_BUILD_CACHE_DIR moves it.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace w2b::cache
{
    bool Enabled();
    std::filesystem::path Root();

    // "path:size:mtime" of a tool file: a replaced tool is a new identity
    std::string FileIdentity(const std::filesystem::path& path);

    // sha256 over the parts, each length-prefixed
    std::string Key(std::initializer_list<std::string_view> parts);

    std::optional<std::string> Load(std::string_view space, const std::string& key);
    void Store(std::string_view space, const std::string& key, std::string_view value);
} // namespace w2b::cache
