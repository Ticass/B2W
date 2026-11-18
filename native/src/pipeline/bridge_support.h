#pragma once

#include "pipeline/modutil.h"

#include <chrono>
#include <optional>

namespace w2b::pipeline::bridge
{
    using mod::Str;
    using py::Json;

    // progress.timed's lines
    template <typename F> auto Timed(const std::string& label, F&& f)
    {
        mod::Print("[staging] " + label + "...");
        const auto started = std::chrono::steady_clock::now();
        auto result = f();
        char text[64];
        std::snprintf(text, sizeof text, "%.1f", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        mod::Print("[staging] " + label + " finished in " + text + "s");
        return result;
    }

    inline Json Paths(const std::vector<fs::path>& paths)
    {
        Json out = Json::array();
        for (const auto& p : paths)
            out.push_back(Str(p));
        return out;
    }

    inline std::vector<fs::path> PathList(const Json& v)
    {
        std::vector<fs::path> out;
        if (v.is_array())
            for (const auto& p : v)
                out.push_back(files::FromUtf8(p.get<std::string>()));
        return out;
    }

    inline std::optional<fs::path> OptionalPath(const Json& v)
    {
        return v.is_string() ? std::optional(files::FromUtf8(v.get<std::string>())) : std::nullopt;
    }

    inline Json PathOrNull(const std::optional<fs::path>& p)
    {
        return p ? Json(Str(*p)) : Json();
    }

    inline bool Truthy(const Json& v)
    {
        if (v.is_null())
            return false;
        if (v.is_string()) // json::empty() is false for every string
            return !v.get_ref<const std::string&>().empty();
        if (v.is_object() || v.is_array())
            return !v.empty();
        if (v.is_boolean())
            return v.get<bool>();
        if (v.is_number())
            return v.get<double>() != 0;
        return true;
    }

    inline void AppendZone(const fs::path& projectRoot, const std::string& lines)
    {
        std::string existing;
        files::ReadBytes(projectRoot / "mod_extra.zone", existing);
        files::WriteBytes(projectRoot / "mod_extra.zone", existing + mod::TextBytes(lines));
    }

    // appends every element of the array `items` to the array report[key]
    inline void Extend(Json& report, const char* key, const Json& items)
    {
        for (const auto& item : items)
            report[key].push_back(item);
    }

    // a private folder under the temporary directory, removed with its contents
    class TemporaryFolder
    {
    public:
        TemporaryFolder();
        ~TemporaryFolder();
        TemporaryFolder(const TemporaryFolder&) = delete;
        TemporaryFolder& operator=(const TemporaryFolder&) = delete;
        const fs::path& Path() const
        {
            return folder_;
        }

    private:
        fs::path folder_;
    };

    // a native step's JsonRequest entry point, called in process: the request
    // is written to a private temporary file the step reads
    Json Call(Json (*step)(const fs::path&), const Json& request);
}

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline::bridge
{
    // the request form of the stock WaW zones a native step continues from
    // ({waw_root, unlinker, work, prepared}); null without them
    Json StockRequest(const extract::StockWaw* stock);
    // takes over the zone dumps a step prepared ({stock_prepared})
    void AdoptPrepared(extract::StockWaw* stock, const Json& result);
}
