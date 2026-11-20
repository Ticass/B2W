#include "sound/stage.h"

#include <algorithm>

namespace w2b::sound
{
    void CheckOutputName(const std::string& kind, std::string name)
    {
        if (name.empty())
            return;
        std::ranges::replace(name, '\\', '/');
        constexpr std::string_view invalid(":,\r\n\0", 5);
        const auto parts = util::Split(name, "/");
        if (name.starts_with('/') || name.find_first_of(invalid) != name.npos ||
            std::ranges::find(parts, "..") != parts.end())
            throw std::invalid_argument("unsafe " + kind + " name: " + util::StringRepr(name));
    }

    std::string AudioName(const util::Json& file)
    {
        std::string joined;
        for (const auto key : {"dir", "name"})
        {
            const auto part = file.find(key);
            if (part == file.end() || part->is_null())
                continue;
            const auto& text = part->get_ref<const std::string&>();
            if (text.empty())
                continue;
            if (!joined.empty())
                joined += '/';
            joined += text;
        }
        std::string name;
        name.reserve(joined.size());
        for (const char raw : joined)
        {
            const char c = raw == '\\' ? '/' : raw;
            if (c != '/' || (!name.empty() && name.back() != '/'))
                name += c;
        }
        if (name.empty())
            throw SoundError("sound file has no name");
        CheckOutputName("audio", name);
        if (name.starts_with("sound/"))
            name.erase(0, 6);
        return name;
    }
}
