#pragma once
// Small helpers the build-mod modules share: text reads and writes,
// str(Path), shutil copies and the list repr of error messages.

#include "core/encoding.h"
#include "core/files.h"
#include "core/text.h"
#include "finish.h"

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::pipeline::mod
{
    namespace fs = std::filesystem;

    // str(path) (backslashes)
    inline std::string Str(fs::path p)
    {
        return files::Utf8(p.make_preferred());
    }

    // path.as_posix()
    inline std::string Generic(const fs::path& p)
    {
        auto s = files::Utf8(p);
        for (auto& c : s)
            if (c == '\\')
                c = '/';
        return s;
    }

    // Path.resolve()
    inline fs::path Resolve(const fs::path& p)
    {
        std::error_code e;
        auto r = fs::weakly_canonical(fs::absolute(p), e);
        return (e ? fs::absolute(p) : r).make_preferred();
    }

    inline bool IsFile(const fs::path& p)
    {
        std::error_code e;
        return fs::is_regular_file(p, e);
    }

    inline bool IsDir(const fs::path& p)
    {
        std::error_code e;
        return fs::is_directory(p, e);
    }

    inline std::string Bytes(const fs::path& p)
    {
        std::string data;
        if (!files::ReadBytes(p, data))
            throw StepError("FileNotFoundError", "[Errno 2] No such file or directory: '" + Str(p) + "'");
        return data;
    }

    // read_text(encoding="utf-8", errors="replace")
    inline std::string ReadReplace(const fs::path& p)
    {
        return encoding::UniversalNewlines(encoding::Utf8Replace(Bytes(p)));
    }

    // read_text(encoding="utf-8")
    inline std::string Read(const fs::path& p)
    {
        if (!IsFile(p))
            throw StepError("FileNotFoundError", "[Errno 2] No such file or directory: '" + Str(p) + "'");
        return files::ReadText(p);
    }

    // read_text(encoding="utf-8-sig")
    inline std::string ReadSig(const fs::path& p)
    {
        auto text = Read(p);
        if (text.rfind("\xEF\xBB\xBF", 0) == 0)
            text.erase(0, 3);
        return text;
    }

    inline util::Json Json(const fs::path& p)
    {
        return util::Json::parse(Read(p));
    }

    // json.loads(path.read_text())[key], owned: a range-for over
    // Json(path)["key"] would iterate a destroyed temporary
    inline util::Json Member(const fs::path& p, const char* key)
    {
        auto data = Json(p);
        return std::move(data.at(key));
    }

    // a JSON value's truth: null, false, 0, "" and empty containers are false
    inline bool Truthy(const util::Json& data, const char* key)
    {
        if (!data.is_object() || !data.contains(key))
            return false;
        const auto& v = data[key];
        if (v.is_boolean())
            return v.get<bool>();
        if (v.is_number())
            return v.get<double>() != 0;
        if (v.is_string())
            return !v.get<std::string>().empty();
        if (v.is_array() || v.is_object())
            return !v.empty();
        return false;
    }

    // captured tool output as text mode reads it
    inline std::string Text(const std::string& out)
    {
        return encoding::UniversalNewlines(encoding::Utf8Replace(out));
    }

    // shutil.copy2
    inline void Copy(const fs::path& source, const fs::path& target)
    {
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
    }

    inline void MakeParent(const fs::path& p)
    {
        fs::create_directories(p.parent_path());
    }

    inline std::vector<std::string> Lines(std::string_view text)
    {
        std::vector<std::string> out;
        for (const auto line : util::SplitLines(text))
            out.emplace_back(line);
        return out;
    }

    inline std::string Join(const std::vector<std::string>& lines, std::string_view sep)
    {
        std::string out;
        for (size_t i = 0; i < lines.size(); i++)
            out += (i ? std::string(sep) : std::string()) + lines[i];
        return out;
    }

    inline std::string Strip(std::string_view s)
    {
        return std::string(util::Strip(s));
    }

    inline bool StartsWith(std::string_view s, std::string_view prefix)
    {
        return s.substr(0, prefix.size()) == prefix;
    }

    inline bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
    }

    inline std::string LStrip(std::string_view s)
    {
        size_t i = 0;
        while (i < s.size() && util::IsSpace(s[i]))
            i++;
        return std::string(s.substr(i));
    }

    // str.split(",")
    inline std::vector<std::string> SplitComma(std::string_view s)
    {
        std::vector<std::string> out;
        for (const auto part : util::Split(s, ","))
            out.emplace_back(part);
        return out;
    }

    // name.split("/")[-1]
    inline std::string Basename(const std::string& s)
    {
        const auto at = s.rfind('/');
        return at == std::string::npos ? s : s.substr(at + 1);
    }

    inline std::string Lower(std::string s)
    {
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    inline std::string Upper(std::string s)
    {
        for (auto& c : s)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        return s;
    }

    // repr(list of str)
    std::string ListRepr(const std::vector<std::string>& items);
    // shutil.copytree(source, target, dirs_exist_ok=True), files in parallel (fastcopy.copytree)
    void CopyTree(const fs::path& source, const fs::path& target);
    // write_if_changed: only when the bytes differ
    void WriteIfChanged(const fs::path& path, const std::string& data);
    // text_bytes: the bytes Path.write_text writes on Windows
    std::string TextBytes(std::string_view text);
    // the progress print of the build steps
    void Print(const std::string& line);
} // namespace w2b::pipeline::mod
