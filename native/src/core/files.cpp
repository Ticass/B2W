#include "files.h"

#include "encoding.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace w2b::files
{
    bool ReadBytes(const fs::path& path, std::string& data)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return false;
        const auto size = static_cast<size_t>(stream.tellg());
        data.resize(size);
        stream.seekg(0);
        stream.read(data.data(), static_cast<std::streamsize>(size));
        return static_cast<bool>(stream) || size == 0;
    }

    void WriteBytes(const fs::path& path, const std::string_view data)
    {
        if (path.has_parent_path())
            fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    void WriteText(const fs::path& path, const std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + text.size() / 32);
        for (const char c : text)
        {
            if (c == '\n')
                out += '\r';
            out += c;
        }
        WriteBytes(path, out);
    }

    std::string ReadText(const fs::path& path)
    {
        std::string bytes;
        if (!ReadBytes(path, bytes))
            throw std::runtime_error("cannot read " + Utf8(path));
        if (encoding::Utf8Replace(bytes) != bytes)
            throw encoding::DecodeError(Utf8(path) + ": not UTF-8");
        return encoding::UniversalNewlines(bytes);
    }

    namespace
    {
        std::wstring Lower(std::wstring text)
        {
            // str.lower() on the ASCII range is what the converter's paths use;
            // CharLowerW-equivalent folding is not needed for game asset names
            for (auto& c : text)
                if (c >= L'A' && c <= L'Z')
                    c = static_cast<wchar_t>(c - L'A' + L'a');
            return text;
        }

        std::vector<std::wstring> Parts(const fs::path& path)
        {
            std::vector<std::wstring> parts;
            for (const auto& part : path)
                parts.push_back(Lower(part.wstring()));
            return parts;
        }
    } // namespace

    std::vector<fs::path> RGlob(const fs::path& folder, const std::string_view suffix)
    {
        std::vector<fs::path> found;
        std::error_code error;
        if (!fs::is_directory(folder, error))
            return found;
        const std::wstring wanted = Lower(fs::path(std::string(suffix)).wstring());
        // folders in the order the walk yields them: a folder's subfolders are
        // yielded when it is visited; the walk visits the last pushed first
        std::vector<fs::path> order{folder}, stack{folder};
        while (!stack.empty())
        {
            const auto current = stack.back();
            stack.pop_back();
            std::vector<fs::path> children;
            for (fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
                if (it->is_directory(error) && !it->is_symlink(error))
                    children.push_back(it->path());
            order.insert(order.end(), children.begin(), children.end());
            stack.insert(stack.end(), children.begin(), children.end());
        }
        for (const auto& dir : order)
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
            {
                if (it->is_directory(error))
                    continue;
                const auto name = Lower(it->path().filename().wstring());
                if (name.size() >= wanted.size() && name.compare(name.size() - wanted.size(), wanted.size(), wanted) == 0)
                    found.push_back(it->path());
            }
        return found;
    }

    std::vector<fs::path> GlobSuffix(const fs::path& folder, const std::string_view suffix)
    {
        std::vector<fs::path> found;
        std::error_code error;
        if (!fs::is_directory(folder, error))
            return found;
        const std::wstring wanted = Lower(fs::path(std::string(suffix)).wstring());
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end; it != end;
             it.increment(error))
        {
            if (error)
                break;
            const auto name = Lower(it->path().filename().wstring());
            if (name.size() >= wanted.size() && name.compare(name.size() - wanted.size(), wanted.size(), wanted) == 0)
                found.push_back(it->path());
        }
        std::vector<std::pair<std::vector<std::wstring>, fs::path>> keyed;
        keyed.reserve(found.size());
        for (auto& path : found)
            keyed.emplace_back(Parts(path), std::move(path));
        std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        found.clear();
        for (auto& [_, path] : keyed)
            found.push_back(std::move(path));
        return found;
    }

    std::string Utf8(const fs::path& path)
    {
        const auto text = path.u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }

    fs::path FromUtf8(const std::string_view text)
    {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    }
} // namespace w2b::files
