#pragma once

#include "core/timeline.h"
#include "pipeline/bridge_report.h"
#include "pipeline/finish.h"
#include "pipeline/modutil.h"
#include "weapons/weapon.h"

#include <chrono>
#include <optional>

namespace w2b::pipeline::bridge
{
    using mod::Str;
    using util::Json;

    // progress.timed's lines, and the step's span in the build timeline
    template <typename F> auto Timed(const std::string& label, F&& f)
    {
        const timeline::Span span(label);
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

    // appends a step's warnings, errors, images or material records to the bridge report
    inline void Extend(BridgeReport& report, const char* key, const Json& items)
    {
        const std::string_view list = key;
        for (const auto& item : items)
            if (list == "warnings")
                report.Warn(item.get<std::string>());
            else if (list == "errors")
                report.Fail(item.get<std::string>());
            else if (list == "images")
                report.images.push_back(item);
            else if (list == "materials")
                report.materials.push_back(MaterialRecord::FromJson(item));
            else
                throw std::invalid_argument("not a bridge report list: " + std::string(list));
    }

    // runs a step, raising its weapon and value errors as the StepErrors the bridge reports
    template <typename F> auto Reported(F&& step)
    {
        try
        {
            return step();
        }
        catch (const weapons::WeaponError& error)
        {
            throw StepError("WeaponError", error.what());
        }
        catch (const std::invalid_argument& error)
        {
            throw StepError("ValueError", error.what());
        }
        catch (const Json::exception& error)
        {
            throw StepError("ValueError", error.what());
        }
    }
}

