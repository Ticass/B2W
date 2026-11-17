#include "weapons/stage.h"

#include "core/files.h"

#include <windows.h>

#include <algorithm>

namespace w2b::weapons
{
    std::string PosixPath(const path& value)
    {
        auto text = files::Utf8(value);
        std::ranges::replace(text, '\\', '/');
        return text;
    }

    std::string NameList(const Names& names)
    {
        std::string result = "[";
        for (const auto& name : names)
        {
            if (result.size() > 1)
                result += ", ";
            result += py::StringRepr(name);
        }
        return result + "]";
    }

    path FindSource(const std::vector<path>& roots, const path& relative)
    {
        for (const auto& root : roots)
        {
            auto candidate = root / relative;
            if (files::fs::is_regular_file(candidate))
                return candidate;
        }
        return {};
    }

    path FindModelLod(const std::vector<path>& roots, const path& owner, const std::string& file)
    {
        OutputName("model_file", file);
        const auto relative = files::FromUtf8(file);
        const auto local = owner / relative;
        return files::fs::is_regular_file(local) ? local : FindSource(roots, relative);
    }

    void CopySource(const path& source, const path& destination)
    {
        files::fs::create_directories(destination.parent_path());
        if (!CopyFileW(source.c_str(), destination.c_str(), FALSE))
            throw files::fs::filesystem_error("could not copy weapon asset", source, destination,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }

    Json ReadAssetJson(const path& source)
    {
        std::string bytes;
        if (!files::ReadBytes(source, bytes))
            throw std::runtime_error("cannot read " + files::Utf8(source));
        return Json::parse(bytes);
    }

    void AddDependency(Json& dependencies, std::string_view kind, const std::string& name)
    {
        auto& names = dependencies[kind];
        if (names.is_null())
            names = Json::array();
        names.push_back(name);
    }

    Names GetDependencies(const Json& dependencies, std::string_view kind)
    {
        const auto group = dependencies.find(kind);
        return group == dependencies.end() ? Names{} : group->get<Names>();
    }
}
