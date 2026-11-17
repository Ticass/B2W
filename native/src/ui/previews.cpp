// Artwork previews (see previews.h). Layout numbers are menupreviews.py's,
// taken from the stock menus.
#include "previews.h"

#include "core/files.h"
#include "theme.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace w2b::ui
{
    namespace l = launcher;
    namespace fs = std::filesystem;

    namespace
    {
        constexpr float SW = 1280, SH = 720, PI = 3.14159265f;
        constexpr float START_W = 400, START_H = 200, TILT = 18.0f, GLOBE = 560;
        const ImU32 ORANGE = IM_COL32(236, 114, 27, 255), OFF_WHITE = IM_COL32(226, 226, 220, 255);

        // menu space -> screen
        struct Canvas
        {
            ImVec2 origin;
            float scale = 1;
            ImVec2 P(const float x, const float y) const
            {
                return ImVec2(origin.x + x * scale, origin.y + y * scale);
            }
        };

        struct Layers
        {
            Texture blit, large, blur;
            bool All() const
            {
                return blit && large && blur;
            }
            bool Aligned() const
            {
                return All() && blit.width == large.width && large.width == blur.width && blit.height == large.height && large.height == blur.height;
            }
        };

        // fit_aspect: the centred crop of a w x h canvas to `aspect`
        ImRect CoverCrop(const float w, const float h, const float aspect)
        {
            if (w / h > aspect)
            {
                const float keep = std::round(h * aspect);
                const float left = std::floor((w - keep) / 2);
                return ImRect(left, 0, left + keep, h);
            }
            const float keep = std::round(w / aspect);
            const float top = std::floor((h - keep) / 2);
            return ImRect(0, top, w, top + keep);
        }

        // draws canvas-space images covering `target` (screen) after a cover crop
        struct Cover
        {
            ImRect target, crop;
            ImVec2 Map(const float x, const float y) const
            {
                return ImVec2(target.Min.x + (x - crop.Min.x) / crop.GetWidth() * target.GetWidth(), target.Min.y + (y - crop.Min.y) / crop.GetHeight() * target.GetHeight());
            }
            void Image(ImDrawList* list, const Texture& texture, const ImRect& at) const
            {
                list->AddImage(reinterpret_cast<ImTextureID>(texture.view), Map(at.Min.x, at.Min.y), Map(at.Max.x, at.Max.y));
            }
        };

        // composite_art then _cover: the art layers filling `target`
        void Composite(ImDrawList* list, const Layers& layers, const ImRect& target, const bool blurred)
        {
            const Texture& background = blurred ? layers.blur : layers.large;
            const bool aligned = layers.Aligned();
            const float cw = static_cast<float>(aligned ? layers.large.width : background.width), ch = static_cast<float>(aligned ? layers.large.height : background.height);
            const Cover cover{target, CoverCrop(cw, ch, target.GetWidth() / target.GetHeight())};
            list->PushClipRect(target.Min, target.Max, true);
            cover.Image(list, background, ImRect(0, 0, cw, ch));
            if (aligned)
                cover.Image(list, layers.blit, ImRect(0, 0, cw, ch));
            else
                cover.Image(list, layers.blit, ImRect(864, 882, 864 + 320, 882 + 284));
            list->PopClipRect();
        }

        void CoverImage(ImDrawList* list, const Texture& texture, const ImRect& target)
        {
            const Cover cover{target, CoverCrop(static_cast<float>(texture.width), static_cast<float>(texture.height), target.GetWidth() / target.GetHeight())};
            list->PushClipRect(target.Min, target.Max, true);
            cover.Image(list, texture, ImRect(0, 0, static_cast<float>(texture.width), static_cast<float>(texture.height)));
            list->PopClipRect();
        }

        float TextWidth(const float size, const std::string& text)
        {
            return theme::Display()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
        }

        void Text(ImDrawList* list, const Canvas& c, const float size, const float x, const float y, const ImU32 color, const std::string& text, const bool right = false)
        {
            const float px = right ? x - TextWidth(size, text) : x;
            list->AddText(theme::Display(), size * c.scale, c.P(px, y), color, text.c_str());
        }

        std::vector<std::string> Wrap(const std::string& text, const float size, const float width)
        {
            std::vector<std::string> lines;
            std::string line, word;
            std::istringstream words(text);
            while (words >> word)
            {
                const auto trial = line.empty() ? word : line + " " + word;
                if (!line.empty() && TextWidth(size, trial) > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                    line = trial;
            }
            if (!line.empty())
                lines.push_back(line);
            return lines;
        }

        std::string Upper(std::string text)
        {
            for (auto& ch : text)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            return text;
        }

        void Placeholder(ImDrawList* list, const Canvas& c, const ImRect& area, const std::string& text)
        {
            list->AddRectFilled(area.Min, area.Max, IM_COL32(21, 26, 32, 255));
            const float w = (area.GetWidth()) / c.scale, h = area.GetHeight() / c.scale;
            const auto lines = Wrap(text, 28, w - 120);
            float y = (area.Min.y - c.origin.y) / c.scale + h / 2 - 20 * static_cast<float>(lines.size());
            const float x0 = (area.Min.x - c.origin.x) / c.scale;
            for (const auto& line : lines)
            {
                Text(list, c, 28, x0 + (w - TextWidth(28, line)) / 2, y, IM_COL32(171, 184, 195, 255), line);
                y += 40;
            }
        }

        void Back(ImDrawList* list, const Canvas& c)
        {
            Text(list, c, 26, 210, 672, IM_COL32(232, 178, 64, 255), "ESC");
            Text(list, c, 26, 262, 672, OFF_WHITE, "Back");
        }

        std::optional<std::pair<float, float>> ParsePoint(const std::string& text)
        {
            const auto comma = text.find(',');
            if (comma == std::string::npos || text.find(',', comma + 1) != std::string::npos)
                return std::nullopt;
            try
            {
                size_t a = 0, b = 0;
                const float x = std::stof(text.substr(0, comma), &a), y = std::stof(text.substr(comma + 1), &b);
                return std::make_pair(x, y);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        // Globe.project: BO2 (lon, lat) -> globe pixels, or none on the far side
        std::optional<ImVec2> Project(const float centerLon, const float bo2Lon, const float lat)
        {
            const float geo = (-bo2Lon - centerLon) * PI / 180, phi = lat * PI / 180, tilt = TILT * PI / 180;
            const float x = std::cos(phi) * std::sin(geo), y = std::sin(phi), z = std::cos(phi) * std::cos(geo);
            const float y2 = y * std::cos(tilt) - z * std::sin(tilt), z2 = y * std::sin(tilt) + z * std::cos(tilt);
            if (z2 <= 0.05f)
                return std::nullopt;
            const float radius = GLOBE / 2 - 2;
            return ImVec2(GLOBE / 2 + x * radius, GLOBE / 2 - y2 * radius);
        }

        // Globe.pick: globe pixels -> BO2 (lon, lat)
        std::optional<std::pair<int, int>> Pick(const float centerLon, const float px, const float py)
        {
            const float radius = GLOBE / 2 - 2, tilt = TILT * PI / 180;
            const float nx = (px - GLOBE / 2) / radius, ny = (GLOBE / 2 - py) / radius, d = nx * nx + ny * ny;
            if (d >= 1)
                return std::nullopt;
            const float nz = std::sqrt(1 - d);
            const float lat = std::asin(std::clamp(ny * std::cos(tilt) + nz * std::sin(tilt), -1.0f, 1.0f)) * 180 / PI;
            const float geo = centerLon + std::atan2(nx, nz * std::cos(tilt) - ny * std::sin(tilt)) * 180 / PI;
            const float bo2 = -(std::fmod(std::fmod(geo + 180, 360) + 360, 360) - 180);
            return std::make_pair(static_cast<int>(std::lround(bo2)), static_cast<int>(std::lround(lat)));
        }

        struct StockMap
        {
            float lon, lat;
            std::string image, name;
        };

        std::vector<StockMap> StockMaps(const std::string& bo2)
        {
            static std::string cachedFor;
            static std::vector<StockMap> cached;
            if (bo2 == cachedFor)
                return cached;
            cachedFor = bo2;
            cached.clear();
            const std::pair<const char*, const char*> names[] = {{"zm_transit", "TranZit"}, {"zm_nuked", "Nuketown"}, {"zm_highrise", "Die Rise"},
                                                                 {"zm_prison", "Mob of the Dead"}, {"zm_buried", "Buried"}, {"zm_tomb", "Origins"}};
            std::string text;
            if (bo2.empty() || !files::ReadBytes(l::Path(bo2) / "raw/zm/mapstable.csv", text))
                return cached;
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::vector<std::string> row;
                std::string cell;
                std::istringstream cells(line);
                while (std::getline(cells, cell, ','))
                    row.push_back(cell);
                if (row.size() <= 17)
                    continue;
                for (const auto& [key, name] : names)
                    if (row[0] == key)
                        try
                        {
                            cached.push_back({std::stof(row[16]), std::stof(row[17]), row[4], name});
                        }
                        catch (const std::exception&)
                        {
                        }
            }
            return cached;
        }

        float Number(const std::string& text)
        {
            try
            {
                return std::stof(text);
            }
            catch (const std::exception&)
            {
                return 0.0f;
            }
        }
    } // namespace

    const Previews::Alpha* Previews::BlitAlpha(const fs::path& path)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        const auto time = ec ? 0 : fs::last_write_time(path, ec).time_since_epoch().count();
        auto& entry = alphas[path.wstring()];
        if (entry.grid.empty() || entry.size != size || entry.time != static_cast<long long>(time))
        {
            entry = {size, static_cast<long long>(time)};
            try
            {
                const auto image = l::DecodeImage(path);
                entry.width = image.width;
                entry.height = image.height;
                // Image.resize((160, 90), BOX) of the alpha channel: area averages
                std::vector<double> sum(160 * 90, 0.0), weight(160 * 90, 0.0);
                for (int y = 0; y < image.height; y++)
                {
                    const int gy = std::min(89, y * 90 / image.height);
                    for (int x = 0; x < image.width; x++)
                    {
                        const int g = gy * 160 + std::min(159, x * 160 / image.width);
                        sum[g] += image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + 3];
                        weight[g] += 1;
                    }
                }
                entry.grid.resize(160 * 90);
                for (size_t g = 0; g < entry.grid.size(); g++)
                    entry.grid[g] = weight[g] ? static_cast<float>(sum[g] / weight[g]) : 0.0f;
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        }
        return &entry;
    }

    bool Previews::Draw(Gpu& gpu, l::Settings& settings)
    {
        static const char* names[] = {"Globe", "Map selection", "Focused selection", "Lobby", "Loading screen"};
        static const char* layerNames[] = {"Large", "Blur", "Blit"};
        bool changed = false;
        ImGui::PushStyleColor(ImGuiCol_Text, theme::GROUP_TITLE);
        ImGui::TextUnformatted("In-game preview");
        ImGui::PopStyleColor();
        for (int i = 0; i < 5; i++)
        {
            if (i)
                ImGui::SameLine();
            // WinForms radio buttons: a 13 px circle
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(theme::S(2), std::max(0.0f, (theme::S(13) - ImGui::GetFontSize()) * 0.5f)));
            if (ImGui::RadioButton(names[i], mode == i))
                mode = i;
            ImGui::PopStyleVar();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(theme::S(80));
        const char* current = mode >= LayerLarge ? layerNames[mode - LayerLarge] : "Layer\xE2\x80\xA6";
        if (ImGui::BeginCombo("##layer", current))
        {
            for (int i = 0; i < 3; i++)
                if (ImGui::Selectable(layerNames[i], mode == LayerLarge + i))
                    mode = LayerLarge + i;
            ImGui::EndCombo();
        }

        // the art, decoded once per file version
        std::vector<std::string> errors, missing;
        Layers layers;
        const auto load = [&](const std::string& path, const char* label, Texture& out, const bool layer)
        {
            if (path.empty())
            {
                if (layer)
                    missing.push_back(label);
                return;
            }
            std::string error;
            out = gpu.Image(l::Path(path), &error);
            if (!error.empty())
                errors.push_back(std::string(label) + ": " + error);
        };
        load(settings.menu_blit, "Blit", layers.blit, true);
        load(settings.menu_large, "Large", layers.large, true);
        load(settings.menu_blur, "Blur", layers.blur, true);
        Texture icon, preview, loading;
        load(settings.menu_icon, "Icon", icon, false);
        load(settings.menu_preview, "Preview", preview, false);
        load(settings.menu_loading, "Loading", loading, false);
        const auto stock = [&](const std::string& name) { return settings.bo2.empty() ? Texture{} : gpu.Dds(l::Path(settings.bo2) / "raw/images" / l::Path(name + ".dds")); };
        const std::string title = !settings.menu_title.empty() ? settings.menu_title : !settings.project.empty() ? settings.project : "Your Map";
        const std::string description = !settings.menu_description.empty() ? settings.menu_description : "Your map description appears here.";

        // the 16:9 menu screen, fitted in the pane
        const float reserve = ImGui::GetTextLineHeightWithSpacing() * 4;
        const ImVec2 avail(ImGui::GetContentRegionAvail().x, std::max(theme::S(180), ImGui::GetContentRegionAvail().y - reserve));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        Canvas c;
        c.scale = std::min(avail.x / SW, avail.y / SH);
        c.origin = ImVec2(std::floor(start.x + (avail.x - SW * c.scale) / 2), std::floor(start.y + (avail.y - SH * c.scale) / 2));
        const ImRect screen(c.P(0, 0), c.P(SW, SH));
        auto* list = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("##screen", avail);
        const bool hovered = ImGui::IsItemHovered();
        list->PushClipRect(screen.Min, screen.Max, true);
        list->AddRectFilled(screen.Min, screen.Max, IM_COL32(12, 14, 18, 255));

        const std::string hints[] = {"Drag to spin the globe. Click it to place your map there.", "Click to place the start location (the box players pick).",
                                     "After picking the start location: Blur behind, sharp Blit, option list.", "", ""};
        if (mode == Globe)
        {
            if (const auto background = stock("menu_zm_background_main"))
                CoverImage(list, background, screen);
            list->AddRectFilled(screen.Min, screen.Max, IM_COL32(0, 0, 0, 90));
            const float gx = (SW - GLOBE) / 2, gy = (SH - GLOBE) / 2 + 10;
            gpu.DrawGlobe(list, stock("globe_map_zm"), c.P(gx, gy).x, c.P(gx, gy).y, GLOBE * c.scale, globeLon, TILT);
            for (int k = 0; k < 3; k++)
                list->AddCircle(c.P(gx + GLOBE / 2, gy + GLOBE / 2), (GLOBE / 2 - 3.5f + k) * c.scale, IM_COL32(255, 140, 60, 70 - k * 20), 128, 2 * c.scale);
            const auto marker = [&](const float lon, const float lat, const Texture& image, const std::string& name, const bool highlighted)
            {
                const auto spot = Project(globeLon, lon, lat);
                if (!spot)
                    return;
                const float x = spot->x + gx, y = spot->y + gy, size = highlighted ? 54.0f : 34.0f;
                if (image)
                {
                    const float s = std::min(1.0f, size / std::max(image.width, image.height));
                    const float w = image.width * s, h = image.height * s;
                    list->AddImage(reinterpret_cast<ImTextureID>(image.view), c.P(x - w / 2, y - h / 2), c.P(x + w / 2, y + h / 2));
                }
                else
                    list->AddCircleFilled(c.P(x, y), 6 * c.scale, highlighted ? ORANGE : OFF_WHITE);
                if (name.empty())
                    return;
                const float face = highlighted ? 18.0f : 13.0f, left = x + size / 2 + 4, width = TextWidth(face, name);
                list->AddRectFilled(c.P(left - 4, y - 12), c.P(left + width + 6, y + 12), IM_COL32(32, 22, 14, 220), 4 * c.scale);
                list->AddRect(c.P(left - 4, y - 12), c.P(left + width + 6, y + 12), highlighted ? IM_COL32(150, 105, 50, 255) : IM_COL32(90, 70, 50, 255), 4 * c.scale);
                Text(list, c, face, left + 1, y - face * 0.6f, highlighted ? ORANGE : OFF_WHITE, name);
            };
            for (const auto& m : StockMaps(settings.bo2))
                marker(m.lon, m.lat, stock(m.image), m.name, false);
            marker(Number(settings.menu_longitude), Number(settings.menu_latitude), icon ? icon : layers.blit, title, true);
            Back(list, c);
        }
        else if (mode == MapSelection || mode == FocusedSelection)
        {
            const bool focused = mode == FocusedSelection;
            if (!layers.All())
                Placeholder(list, c, screen, "Upload Blit, Large and Blur to preview the map selection screen.");
            else
            {
                Composite(list, layers, screen, focused);
                float cx = 0, cy = 0;
                if (layers.Aligned())
                    if (const auto* alpha = BlitAlpha(l::Path(settings.menu_blit)))
                    {
                        // blit_window: the 400 x 200 start box, at the point or the densest Blit
                        const int cols = static_cast<int>(std::lround(START_W / 1280 * 160)), rows = static_cast<int>(std::lround(START_H / 720 * 90));
                        int left = 0, top = 0;
                        if (const auto point = ParsePoint(settings.menu_location_point))
                        {
                            left = std::clamp(static_cast<int>(std::lround(point->first / alpha->width * 160 - cols / 2.0)), 0, 160 - cols);
                            top = std::clamp(static_cast<int>(std::lround(point->second / alpha->height * 90 - rows / 2.0)), 0, 90 - rows);
                        }
                        else
                        {
                            double best = -1;
                            for (int y0 = 0; y0 <= 90 - rows; y0++)
                                for (int x0 = 0; x0 <= 160 - cols; x0++)
                                {
                                    double total = 0;
                                    for (int y = 0; y < rows; y++)
                                        for (int x = 0; x < cols; x++)
                                            total += alpha->grid[static_cast<size_t>((y0 + y) * 160 + x0 + x)];
                                    if (total > best)
                                        best = total, left = x0, top = y0;
                                }
                        }
                        cx = std::round((left + cols / 2.0f) / 160 * 1280 - 640);
                        cy = std::round((top + rows / 2.0f) / 90 * 720 - 360);
                    }
                const float x = SW / 2 + cx, y = SH / 2 + cy;
                if (!focused)
                {
                    for (int k = 4; k >= 1; k--)
                        list->AddRect(c.P(x - START_W / 2 - k * 1.5f, y - START_H / 2 - k * 1.5f), c.P(x + START_W / 2 + k * 1.5f, y + START_H / 2 + k * 1.5f),
                                      IM_COL32(255, 170, 60, 40), 0, 3 * c.scale);
                    list->AddRect(c.P(x - START_W / 2, y - START_H / 2), c.P(x + START_W / 2, y + START_H / 2), IM_COL32(255, 190, 90, 230), 0, 2 * c.scale);
                }
                const float left = std::min(std::max(SW / 2 + cx + 180, 20.0f), SW - 360), top = std::min(std::max(SH / 2 + cy - 50, 20.0f), SH - 160);
                const auto label = Upper(title);
                list->AddRect(c.P(left - 6, top - 4), c.P(left + TextWidth(30, label) + 6, top + 38), ORANGE, 0, 2 * c.scale);
                Text(list, c, 30, left, top, ORANGE, label);
                const auto lines = Wrap(description, 24, 320);
                for (size_t i = 0; i < std::min<size_t>(4, lines.size()); i++)
                {
                    if (i == 0)
                        list->AddTriangleFilled(c.P(left + 2, top + 58), c.P(left + 2, top + 72), c.P(left + 10, top + 65), OFF_WHITE);
                    Text(list, c, 24, left + 18, top + 50 + static_cast<float>(i) * 28, OFF_WHITE, lines[i]);
                }
                Back(list, c);
            }
        }
        else if (mode == Lobby)
        {
            if (layers.All())
                Composite(list, layers, screen, true);
            else if (layers.large)
                CoverImage(list, layers.large, screen);
            else
                list->AddRectFilled(screen.Min, screen.Max, IM_COL32(28, 32, 36, 255));
            list->AddRectFilled(screen.Min, screen.Max, IM_COL32(0, 0, 0, 110));
            Text(list, c, 54, 205, 40, IM_COL32(240, 240, 236, 255), "CUSTOM GAMES");
            const char* menu[] = {"START MATCH", "MAP", "", "DIFFICULTY", "STARTING ROUND"};
            for (int i = 0; i < 5; i++)
                Text(list, c, 26, 205, 100 + i * 32.0f, IM_COL32(240, 240, 236, 255), menu[i]);
            const float scale = 1.45f, fw = std::round(294 * scale), fh = std::round(182 * scale), ox = 212, oy = SH - 60 - fh;
            const ImRect card(c.P(ox, oy), c.P(ox + fw, oy + fh));
            if (preview)
                CoverImage(list, preview, card);
            else if (layers.All())
                Composite(list, layers, card, false);
            else
            {
                const Canvas inner{c.origin, c.scale};
                Placeholder(list, inner, card, "Upload a lobby preview");
            }
            if (const auto frame = stock("menu_zm_map_frame"))
                list->AddImage(reinterpret_cast<ImTextureID>(frame.view), card.Min, card.Max);
            else
            {
                list->AddRectFilled(c.P(ox, oy + fh - std::round(46 * scale)), card.Max, IM_COL32(0, 0, 0, 120));
                list->AddRect(card.Min, card.Max, IM_COL32(150, 150, 150, 255), 0, 3 * c.scale);
            }
            Text(list, c, 27, ox + fw - 16, oy + fh - 64, IM_COL32(245, 245, 240, 255), Upper(title), true);
            Text(list, c, 27, ox + fw - 16, oy + fh - 36, IM_COL32(245, 245, 240, 255), Upper(settings.menu_mode.empty() ? title : settings.menu_mode), true);
        }
        else if (mode == Loading)
        {
            if (loading)
                CoverImage(list, loading, screen);
            else if (layers.All())
                Composite(list, layers, screen, false);
            if (!loading && !layers.All())
                Placeholder(list, c, screen, "Upload a loading screen image (or Blit, Large and Blur).");
            else
            {
                list->AddRectFilledMultiColor(c.P(0, SH - 190), c.P(SW, SH - 120), 0, 0, IM_COL32(0, 0, 0, 130), IM_COL32(0, 0, 0, 130));
                list->AddRectFilled(c.P(0, SH - 120), c.P(SW, SH), IM_COL32(0, 0, 0, 130));
                Text(list, c, 52, 60, SH - 132, IM_COL32(245, 245, 240, 255), Upper(title));
                Text(list, c, 26, 62, SH - 74, OFF_WHITE, settings.menu_location.empty() ? "World at War" : settings.menu_location);
                Text(list, c, 24, SW - 60, SH - 60, OFF_WHITE, "LOADING...", true);
            }
        }
        else
        {
            const Texture& layer = mode == LayerLarge ? layers.large : mode == LayerBlur ? layers.blur : layers.blit;
            if (layer)
            {
                const float s = std::min(screen.GetWidth() / layer.width, screen.GetHeight() / layer.height);
                const ImVec2 size(layer.width * s, layer.height * s), at(screen.Min.x + (screen.GetWidth() - size.x) / 2, screen.Min.y + (screen.GetHeight() - size.y) / 2);
                list->AddImage(reinterpret_cast<ImTextureID>(layer.view), at, ImVec2(at.x + size.x, at.y + size.y));
            }
            else
                Placeholder(list, c, screen, std::string("Upload ") + layerNames[mode - LayerLarge] + " to preview it.");
        }
        list->PopClipRect();

        // clicks: spin or place on the globe, place the start location
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float mx = (mouse.x - c.origin.x) / c.scale, my = (mouse.y - c.origin.y) / c.scale;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            dragging = true, moved = false, dragStartX = mouse.x, dragStartLon = globeLon;
        if (dragging && mode == Globe && ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::fabs(mouse.x - dragStartX) > 3)
            moved = true, globeLon = dragStartLon - (mouse.x - dragStartX) / c.scale * 0.35f;
        if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            dragging = false;
            if (!moved && mx >= 0 && my >= 0 && mx < SW && my < SH)
            {
                if (mode == Globe)
                {
                    if (const auto spot = Pick(globeLon, mx - (SW - GLOBE) / 2, my - ((SH - GLOBE) / 2 + 10)))
                    {
                        settings.menu_longitude = std::to_string(spot->first);
                        settings.menu_latitude = std::to_string(spot->second);
                        changed = true;
                    }
                }
                else if (mode == MapSelection && layers.Aligned())
                {
                    // screen_to_canvas: the inverse of the map-selection cover crop
                    const float w = static_cast<float>(layers.large.width), h = static_cast<float>(layers.large.height);
                    const ImRect crop = CoverCrop(w, h, SW / SH);
                    const float x = crop.Min.x + mx / SW * crop.GetWidth(), y = crop.Min.y + my / SH * crop.GetHeight();
                    settings.menu_location_point = std::to_string(std::lround(std::clamp(x, 0.0f, w - 1))) + "," + std::to_string(std::lround(std::clamp(y, 0.0f, h - 1)));
                    changed = true;
                }
            }
        }

        // hint and layer status, as the former launcher wrote them
        ImGui::PushStyleColor(ImGuiCol_Text, theme::MUTED);
        ImGui::PushTextWrapPos(0.0f);
        if (mode < 3)
            ImGui::TextUnformatted(hints[mode].c_str());
        std::string status;
        for (const auto& e : errors)
            status += (status.empty() ? "" : "\n") + e;
        if (status.empty())
        {
            std::string list2;
            for (const auto& m : missing)
                list2 += (list2.empty() ? "" : ", ") + m;
            const bool stockLayout = layers.All() && layers.blit.width == 512 && layers.blit.height == 256 && layers.large.width == 2048 && layers.large.height == 2048
                                     && layers.blur.width == 2048 && layers.blur.height == 2048;
            status = !missing.empty()                          ? "Map selection layers still needed: " + list2
                     : layers.Aligned() || stockLayout         ? "Map selection layers use a supported layout."
                                                               : "Warning: Layer canvas sizes differ. They will be resized to the same canvas, but objects may not line up and "
                                                                 "the result may look distorted. You can still preview and build.";
        }
        ImGui::TextUnformatted(status.c_str());
        ImGui::TextUnformatted("Approximate stock layout drawn with your BO2 install's own menu textures. Check final framing in game.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        return changed;
    }
} // namespace w2b::ui
