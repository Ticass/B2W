// Shader binding outputs (see output.h).
#include "shaders/output.h"

#include "core/files.h"

#include <algorithm>
#include <cctype>

namespace w2b::shaders
{
    namespace fs = std::filesystem;

    namespace
    {
        // str(path) lower-cased: Windows compares paths without case
        std::string Key(fs::path relative)
        {
            auto text = files::Utf8(relative.make_preferred());
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string Posix(const fs::path& relative)
        {
            auto text = files::Utf8(relative);
            std::ranges::replace(text, '\\', '/');
            return text;
        }
    } // namespace

    void FolderOutput::Write(const fs::path& relative, const std::string_view data, const bool text)
    {
        const auto path = root_ / relative;
        if (text)
            files::WriteText(path, data);
        else
            files::WriteBytes(path, data);
    }

    std::optional<std::string> FolderOutput::Read(const fs::path& relative) const
    {
        std::string data;
        if (!files::ReadBytes(root_ / relative, data))
            return std::nullopt;
        return data;
    }

    void MemoryOutput::Write(const fs::path& relative, const std::string_view data, const bool text)
    {
        files_[Key(relative)] = {Posix(relative), text ? files::TextBytes(data) : std::string(data)};
    }

    std::optional<std::string> MemoryOutput::Read(const fs::path& relative) const
    {
        const auto found = files_.find(Key(relative));
        if (found == files_.end())
            return std::nullopt;
        return found->second.second;
    }

    std::vector<std::pair<std::string, std::string>> MemoryOutput::Files() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(files_.size());
        for (const auto& [_, file] : files_)
            out.push_back(file);
        return out;
    }
} // namespace w2b::shaders
