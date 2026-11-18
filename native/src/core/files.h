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
    // the bytes WriteText writes: text with Windows line endings
    std::string TextBytes(std::string_view text);
    // Path.read_text(encoding="utf-8"): strict UTF-8, universal newlines;
    // throws std::runtime_error when unreadable or not UTF-8
    std::string ReadText(const fs::path& path);

    // Files under folder whose name ends in suffix (case-insensitive, as
    // Path.rglob('*' + suffix) on Windows), ordered as sorted(Path) orders them:
    // by lower-cased path components.
    std::vector<fs::path> GlobSuffix(const fs::path& folder, std::string_view suffix);
    // Path.rglob("*<suffix>") in CPython 3.14's order: each folder's files in
    // directory order, folders as its stack walk yields them (unsorted)
    std::vector<fs::path> RGlob(const fs::path& folder, std::string_view suffix);

    std::string Utf8(const fs::path& path);
    fs::path FromUtf8(std::string_view text);
} // namespace w2b::files
