#include "pipeline/menu_stage.h"

#include "core/files.h"
#include "gsc/port/util.h"
#include "pipeline/menu_artwork.h"
#include "pipeline/menu_runtime.h"
#include "pipeline/menu_song.h"

#include <cmath>

namespace w2b::pipeline
{
    namespace
    {
        using util::Json;
        namespace fs = std::filesystem;

        double Coordinate(std::string_view value, std::string_view label, int limit)
        {
            double number = 0;
            if (!util::ParseFloat(value, number) || !std::isfinite(number) || std::abs(number) > limit)
                throw std::invalid_argument(std::string(label) + " must be a number between " +
                                            std::to_string(-limit) + " and " + std::to_string(limit) + ".");
            return number;
        }

        void ClearArtwork(const fs::path& root, const Json& previous)
        {
            for (const auto& name : previous.value("desktop_files", Json::array()))
            {
                const auto relative = files::FromUtf8(name.get_ref<const std::string&>());
                if ((relative.parent_path() == "images" || relative.parent_path() == "materials") &&
                    (relative.extension() == ".iwi" || relative.extension() == ".json"))
                    fs::remove(root / relative);
            }
            ClearMenuRuntime(root, previous);
        }

        Json Metadata(const launcher::Settings& settings, Json previous, double longitude, double latitude)
        {
            for (const auto key : {"icon", "blit", "desktop_files", "loading_song", "layer_layout", "layer_rawfiles",
                                   "layer_materials", "texture_report", "start_location", "preview_layout", "preview_rawfiles"})
                previous.erase(key);
            const auto title = gsc::port::PyStrip(settings.menu_title);
            const auto description = gsc::port::PyStrip(settings.menu_description);
            previous["title"] = title.empty() ? settings.project : title;
            previous["description"] = description.empty() ? settings.project + ": World at War custom map" : description;
            previous["longitude"] = longitude;
            previous["latitude"] = latitude;
            previous["desktop_authored"] = true;
            if (!settings.menu_mode.empty())
                previous["mode"] = settings.menu_mode;
            if (!settings.menu_location.empty())
                previous["location"] = settings.menu_location;
            return previous;
        }

        fs::path MenuCompatPath()
        {
            const auto resources = launcher::ResourceRoot();
            for (const auto& path : {resources / "src/waw2bo2/compat", resources / "waw2bo2/compat"})
                if (fs::is_regular_file(path / "menu_preview.luac"))
                    return path;
            throw std::runtime_error("The menu Lua wrappers are missing.");
        }
    }

    std::string StageMenuSettings(const launcher::Settings& settings, const fs::path& root, const fs::path& decoder,
                                  const fs::path& compat)
    {
        auto artwork = LoadMenuArtwork(settings, decoder);
        const auto longitude = Coordinate(settings.menu_longitude, "Longitude", 180);
        const auto latitude = Coordinate(settings.menu_latitude, "Latitude", 90);
        ValidateMenuSong(settings);
        const auto file = root / "menu.json";
        auto previous = fs::is_regular_file(file) ? Json::parse(files::ReadText(file)) : Json::object();
        const bool authored = previous.value("desktop_authored", false);
        if (authored)
            ClearArtwork(root, previous);
        const auto active = !settings.menu_title.empty() || !settings.menu_description.empty() ||
                            !settings.menu_mode.empty() || !settings.menu_location.empty() ||
                            !settings.loading_song.empty() || longitude != 0 || latitude != 0 || artwork.has_value();
        if (!active)
        {
            StageMenuSong(settings, root, decoder);
            if (authored)
                fs::remove(file);
            return {};
        }
        auto data = Metadata(settings, std::move(previous), longitude, latitude);
        std::string warning;
        if (artwork)
        {
            warning = std::move(artwork->warning);
            StageMenuArtwork(settings, root, compat.empty() ? MenuCompatPath() : compat, decoder, std::move(*artwork), data);
        }
        else
            data["desktop_files"] = Json::array();
        auto song = StageMenuSong(settings, root, decoder);
        if (!song.empty())
            data["loading_song"] = std::move(song);
        files::WriteText(file, util::JsonDumps(data, 2) + "\n");
        return warning;
    }
}
