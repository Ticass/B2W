// Light definitions and the window barrier (see lighting_tail.h).
#include "lighting_tail.h"

#include "barriers/barriers.h"
#include "core/encoding.h"
#include "modutil.h"
#include "textures/iwi.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::StartsWith;
    using mod::Str;
    using py::Json;

    namespace
    {
        std::optional<fs::path> Find(const std::vector<fs::path>& roots, const fs::path& relative)
        {
            std::error_code e;
            for (const auto& root : roots)
                if (fs::exists(root / relative, e))
                    return root / relative;
            return std::nullopt;
        }

        // str() of a JSON number as Python prints it
        std::string NumberText(const Json& v)
        {
            if (v.is_number_integer())
                return std::to_string(v.get<long long>());
            if (v.is_number_float())
                return py::FloatRepr(v.get<double>());
            return v.is_string() ? v.get<std::string>() : v.dump();
        }

        void CopyFile(const fs::path& from, const fs::path& to)
        {
            fs::create_directories(to.parent_path());
            fs::copy_file(from, to, fs::copy_options::overwrite_existing);
            fs::last_write_time(to, fs::last_write_time(from));
        }
    } // namespace

    Json StageLightDefs(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        std::vector<fs::path> roots;
        for (const auto& r : request["roots"])
            roots.push_back(files::FromUtf8(r.get<std::string>()));
        std::set<std::string> definitions;
        for (const auto& d : request["definitions"])
            definitions.insert(d.get<std::string>());
        Json images = Json::array(), errors = Json::array();
        std::set<std::string> attenuation;
        for (const auto& name : definitions)
        {
            const auto sourceDef = Find(roots, fs::path("lightdef") / files::FromUtf8(name + ".json"));
            if (!sourceDef)
            {
                errors.push_back("source light definition " + name + " is missing; re-extract lightdef assets");
                continue;
            }
            auto definition = mod::Json(*sourceDef);
            auto image = definition.at("attenuation").get<std::string>();
            if (StartsWith(image, ","))
                image = image.substr(1);
            if (!image.empty())
            {
                const auto sourceImage = Find(roots, fs::path("images") / files::FromUtf8(image + ".dds"));
                if (!sourceImage)
                {
                    errors.push_back("light attenuation image " + image + " is missing from source dumps");
                    continue;
                }
                const auto output = "waw_light/" + image;
                // T6 binds the ramp with its own (mipmapped) sampler; WaW's def samples it unmipped
                files::WriteBytes(projectRoot / "images" / files::FromUtf8(output + ".iwi"), textures::DdsToIwi(mod::Bytes(*sourceImage), false, true).bytes);
                definition["attenuation"] = output;
                images.push_back(output);
                attenuation.insert(output);
            }
            definition["name"] = "waw_light/" + name;
            files::WriteText(projectRoot / "lightdef/waw_light" / files::FromUtf8(name + ".json"), py::JsonDumps(definition, 1) + "\n");
        }
        // A light def's ramp is a code texture no material requests: stream it as effect images are
        if (!attenuation.empty())
        {
            const auto path = projectRoot / "images" / "streaming.json";
            Json modes = mod::IsFile(path) ? mod::Json(path).at("streamingMode") : Json::object();
            for (const auto& n : attenuation)
                modes[n] = 2;
            std::map<std::string, Json> sorted;
            for (const auto& [k, v] : modes.items())
                sorted[k] = v;
            Json ordered = Json::object();
            for (const auto& [k, v] : sorted)
                ordered[k] = v;
            files::WriteText(path, py::JsonDumps(Json{{"streamingMode", ordered}}, 2) + "\n");
        }
        return Json{{"images", images}, {"errors", errors}};
    }

    Json StageZbarrier(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        const std::string asset = "zmcore_basicwoodbarrier";
        Json warnings = Json::array(), errors = Json::array(), content = Json::object();
        const auto stock = request["techset_dump"].is_null() ? std::optional<fs::path>() : std::optional(files::FromUtf8(request["techset_dump"].get<std::string>()) / "zbarrier" / asset);
        if (stock && fs::exists(*stock))
        {
            auto text = encoding::UniversalNewlines(encoding::Utf8Replace(mod::Bytes(*stock)));
            // The bridge has no T6 FX loader: the stock barrier's repair effects would fail to link.
            const std::string effect = "\\impacts/fx_large_woodhit";
            for (size_t at; (at = text.find(effect)) != std::string::npos;)
                text.replace(at, effect.size(), "\\");
            files::WriteText(projectRoot / "zbarrier" / asset, text);
            warnings.push_back("stock T6 barrier repair FX omitted (bridge has no T6 FX loader)");
            const auto override = projectRoot / "mod_override";
            std::error_code e;
            fs::remove_all(override / "xanim", e);
            const auto& timing = request["timing"];
            if (!timing.is_null() && !request["bo2_root"].is_null())
            {
                const auto written = barriers::StageRepairs(text, files::FromUtf8(request["bo2_root"].get<std::string>()), timing, override);
                content["window_repair_animations"] = Json{{"timing", timing}, {"animations", written}};
                warnings.push_back("WINDOW_REBUILD " + std::to_string(written.size()) + " board repair animations play the map's " + timing.at("script").get<std::string>() +
                                   "::replace_chunk motion (lift " + NumberText(timing.at("lift")) + "s, turn " + NumberText(timing.at("turn")) + "s, hold " +
                                   NumberText(timing.at("hold")) + "s, push " + NumberText(timing.at("push")) + "s); the fast-reload perk scales it as BO2 does");
            }
            else
                warnings.push_back("FRAMEWORK_BO2 window rebuild: no replace_chunk in the map's scripts; the boards keep BO2's repair animation");
            // zm_test's mod.ff already owns this asset: a second copy in the map zone is an override error
            warnings.push_back("T6 barrier asset supplied by zm_test mod.ff");
        }
        else
            errors.push_back("stock T6 zbarrier " + asset + " missing from techset dump");
        return Json{{"warnings", warnings}, {"errors", errors}, {"content", content}};
    }
} // namespace w2b::pipeline
