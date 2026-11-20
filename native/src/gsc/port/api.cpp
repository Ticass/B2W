#include "api.h"

#include "core/files.h"
#include "gsc/tokens.h"

#include <stdexcept>

namespace w2b::gsc::port
{
    namespace
    {
        std::filesystem::path g_compat;
    } // namespace

    void SetCompatDir(const std::filesystem::path& folder)
    {
        g_compat = folder;
    }

    const std::filesystem::path& CompatDir()
    {
        if (g_compat.empty())
            throw std::runtime_error("compat folder not set");
        return g_compat;
    }

    const util::Json& Api()
    {
        static const util::Json api = []
        {
            std::string text;
            if (!files::ReadBytes(CompatDir() / "gsc_api.json", text))
                throw std::runtime_error("missing compat/gsc_api.json");
            return util::Json::parse(text);
        }();
        return api;
    }

    std::string Norm(std::string path)
    {
        for (auto& c : path)
            if (c == '/')
                c = '\\';
        path = Lower(path);
        for (const char* suffix : {".gsc", ".csc"})
        {
            const std::string_view s(suffix);
            if (path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0)
                path.resize(path.size() - s.size());
        }
        return path;
    }
} // namespace w2b::gsc::port
