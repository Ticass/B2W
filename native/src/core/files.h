#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::files
{
    namespace fs = std::filesystem;

    bool ReadBytes(const fs::path& path, std::string& data);

    // Path.write_bytes / Path.write_text(..., encoding='utf-8') on Windows,
    // where text mode turns every "\n" into "\r\n". Creates parent folders.
    void WriteBytes(const fs::path& path, std::string_view data);
    void WriteText(const fs::path& path, std::string_view text);

    // Files under folder whose name ends in suffix (case-insensitive, as
    // Path.rglob('*' + suffix) on Windows), ordered as sorted(Path) orders them:
    // by lower-cased path components.
    std::vector<fs::path> GlobSuffix(const fs::path& folder, std::string_view suffix);

    std::string Utf8(const fs::path& path);
    fs::path FromUtf8(std::string_view text);
} // namespace w2b::files
