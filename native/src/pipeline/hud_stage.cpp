// WaW's engine-owned HUD assets (see hud_stage.h).
#include "hud_stage.h"

#include "assets/resolver.h"
#include "core/unicode.h"
#include "csvtable.h"
#include "extract/stockwaw.h"
#include "menu_artwork.h"
#include "modutil.h"
#include "textures/iwi.h"
#include "textures/pillow_decode.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

namespace w2b::pipeline
{
    using mod::IsFile;
    using mod::StartsWith;
    using py::Json;

    namespace
    {
        const std::vector<std::string> ASSETS{"ammo_counter_bullet", "ammo_counter_riflebullet", "ammo_counter_beltbullet", "ammo_counter_rocket",
                                              "ammo_counter_shotgunshell", "ammo_counter_tesla", "scorebar_zom_1", "scorebar_zom_2",
                                              "scorebar_zom_3", "scorebar_zom_4", "scorebar_zom_long_1", "scorebar_zom_long_2",
                                              "scorebar_zom_long_3", "scorebar_zom_long_4"};

        // wawassets._zone_rank
        std::pair<int, std::string> ZoneRank(const std::string& zone)
        {
            return {StartsWith(zone, "nazi_zombie") ? 0 : StartsWith(zone, "localized") ? 2 : 1, zone};
        }

        // wawhud.SourceFinder over assetresolve.Resolver(roots, stock)
        class SourceFinder
        {
        public:
            SourceFinder(std::vector<fs::path> roots, extract::StockWaw* stock) : roots_(std::move(roots)), stock_(stock)
            {
            }

            std::optional<fs::path> Find(const std::string& kind, const std::string& name)
            {
                const auto relative = assets::SourcePath({kind, name});
                for (const auto& root : Roots())
                    if (IsFile(root / relative))
                        return root / relative;
                // Resolver.find
                for (const auto& root : roots_)
                    if (IsFile(root / relative))
                        return root / relative;
                if (!stock_)
                    return std::nullopt;
                const auto hit = stock_->RootFor(kind, name);
                if (!hit)
                    return std::nullopt;
                stockRoots_[hit->first] = hit->second;
                if (IsFile(hit->second / relative))
                    return hit->second / relative;
                return std::nullopt;
            }

            std::optional<fs::path> MaterialImage(const std::string& name)
            {
                const auto path = Find("material", name);
                if (!path)
                    return std::nullopt;
                const auto data = mod::Json(*path);
                const auto textures = data.value("textures", Json::array());
                for (const auto& t : textures)
                    if (t.contains("name") && t["name"] == "colorMap")
                    {
                        const auto image = t.at("image");
                        if (!image.is_string() || image.get<std::string>().empty())
                            return std::nullopt;
                        return Find("image", image.get<std::string>());
                    }
                return std::nullopt;
            }

            std::optional<fs::path> Font(const std::string& fontName)
            {
                for (const auto& root : roots_)
                    if (IsFile(root / files::FromUtf8(fontName + ".json")))
                        return root / files::FromUtf8(fontName + ".json");
                if (stock_)
                    if (const auto found = stock_->RootFor("font", fontName))
                        if (IsFile(found->second / files::FromUtf8(fontName + ".json")))
                            return found->second / files::FromUtf8(fontName + ".json");
                return std::nullopt;
            }

        private:
            std::vector<fs::path> Roots() const
            {
                auto out = roots_;
                std::vector<std::string> zones;
                for (const auto& [zone, _] : stockRoots_)
                    zones.push_back(zone);
                std::sort(zones.begin(), zones.end(), [](const std::string& a, const std::string& b) { return ZoneRank(a) < ZoneRank(b); });
                for (const auto& zone : zones)
                    out.push_back(stockRoots_.at(zone));
                return out;
            }

            std::vector<fs::path> roots_;
            extract::StockWaw* stock_;
            std::map<std::string, fs::path> stockRoots_;
        };

        struct Missing : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        // str() of a JSON number as csv.writer writes it
        std::string Cell(const Json& v)
        {
            if (v.is_number_integer())
                return std::to_string(v.get<long long>());
            if (v.is_number_float())
                return py::FloatRepr(v.get<double>());
            if (v.is_boolean())
                return v.get<bool>() ? "True" : "False";
            if (v.is_string())
                return v.get<std::string>();
            if (v.is_null())
                return "";
            return v.dump();
        }

        unsigned BitLength(const long long v)
        {
            return v <= 0 ? 0 : static_cast<unsigned>(std::bit_width(static_cast<unsigned long long>(v)));
        }

        std::string Hex4(const unsigned v)
        {
            char text[16];
            std::snprintf(text, sizeof text, "%04x", v);
            return text;
        }

        class Stager
        {
        public:
            explicit Stager(fs::path project) : project_(std::move(project))
            {
            }

            // write_sprite: a lossless RGBA 2D material
            std::string Sprite(const std::string& shortName, const textures::RgbaImage& image)
            {
                const auto name = "waw_hud/" + shortName;
                auto file = name;
                std::replace(file.begin(), file.end(), '*', '_');
                const auto target = project_ / "content_source" / "images" / files::FromUtf8(file + ".iwi");
                files::WriteBytes(target, textures::RgbaToIwi(image.width, image.height, image.pixels).bytes);
                const auto material = project_ / "content_source" / assets::SourcePath({"material", name});
                files::WriteText(material, py::JsonDumpsDefault(MenuMaterial(name)));
                materials.push_back(name);
                return name;
            }

            std::vector<std::string> materials;

        private:
            fs::path project_;
        };

        // font_glyphs: glyph table rows of a source font
        std::vector<std::vector<std::string>> FontGlyphs(SourceFinder& source, const std::string& fontName, const std::string& role, Stager& stager)
        {
            const auto path = source.Font(fontName);
            if (!path)
                throw Missing(fontName);
            const auto font = mod::Json(*path);
            const auto materialName = font.at("material").get<std::string>();
            const auto material = source.Find("material", materialName);
            if (!material)
                throw Missing(materialName);
            const auto imageName = mod::Json(*material).at("textures").at(0).at("image").get<std::string>();
            const auto atlasPath = source.Find("image", imageName);
            if (!atlasPath)
                throw Missing(imageName);
            std::vector<std::vector<std::string>> rows;
            const auto atlas = textures::DecodeDdsPillow(mod::Bytes(*atlasPath));
            for (const auto& glyph : font.at("glyphs"))
            {
                const auto& letter = glyph.at("letter");
                char32_t code = 0;
                if (letter.is_number_integer())
                    code = static_cast<char32_t>(letter.get<long long>());
                else if (letter.is_string() && unicode::Decode(letter.get<std::string>()).size() == 1)
                    code = unicode::Decode(letter.get<std::string>())[0];
                else
                    throw StepError("ValueError", "Invalid source font glyph in " + mod::Str(*path));
                const auto w = glyph.at("pixelWidth").get<long long>(), h = glyph.at("pixelHeight").get<long long>();
                std::string name;
                long long pw = 0, ph = 0;
                if (w && h)
                {
                    const auto x = static_cast<long long>(std::nearbyint(glyph.at("s0").get<double>() * atlas.width));
                    const auto y = static_cast<long long>(std::nearbyint(glyph.at("t0").get<double>() * atlas.height));
                    pw = std::max(4LL, 1LL << BitLength(w - 1));
                    ph = std::max(4LL, 1LL << BitLength(h - 1));
                    const auto cell = atlas.Cell(static_cast<unsigned>(x), static_cast<unsigned>(y), static_cast<unsigned>(w), static_cast<unsigned>(h),
                                                 static_cast<unsigned>(pw), static_cast<unsigned>(ph));
                    name = stager.Sprite(role + "_" + Hex4(static_cast<unsigned>(code)), cell);
                }
                rows.push_back({role + ":" + std::to_string(static_cast<unsigned long>(code)), name, Cell(glyph.at("x0")), Cell(glyph.at("y0")),
                                Cell(glyph.at("dx")), std::to_string(pw), std::to_string(ph), Cell(font.at("pixelHeight"))});
            }
            return rows;
        }
    } // namespace

    Json StageWawHud(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto project = files::FromUtf8(request["project_root"].get<std::string>());
        std::vector<fs::path> roots;
        for (const auto& r : request["roots"])
            roots.push_back(files::FromUtf8(r.get<std::string>()));
        auto stock = extract::StockWaw::FromJson(request.value("stock_waw", Json()));
        SourceFinder source(roots, stock ? &*stock : nullptr);
        Stager stager(project);
        std::vector<std::string> missing;
        for (const auto& name : ASSETS)
        {
            const auto path = source.MaterialImage(name);
            if (!path)
            {
                // Tesla is DLC-owned and absent on maps which do not ship the weapon.
                if (name != "ammo_counter_tesla")
                    missing.push_back(name);
                continue;
            }
            stager.Sprite(name, textures::DecodeDdsPillow(mod::Bytes(*path)));
        }
        std::vector<std::vector<std::string>> glyphRows;
        std::vector<std::string> fonts;
        for (const auto& [role, fontName] : std::vector<std::pair<std::string, std::string>>{{"objective", "fonts/objectiveFont"}, {"score", "fonts/normalFont"}})
        {
            try
            {
                auto rows = FontGlyphs(source, fontName, role, stager);
                glyphRows.insert(glyphRows.end(), rows.begin(), rows.end());
            }
            catch (const Missing& error)
            {
                missing.push_back(error.what());
                continue;
            }
            fonts.push_back(fontName);
        }
        if (!missing.empty())
        {
            std::string names;
            for (const auto& m : missing)
                names += (names.empty() ? "" : ", ") + m;
            throw StepError("ValueError", "WaW HUD source assets missing: " + names);
        }
        files::WriteText(project / "zm/waw_hud_glyphs.csv", CsvText(glyphRows, "\n"));
        const Json result{{"materials", stager.materials}, {"fonts", fonts}, {"glyphs", glyphRows.size()}};
        files::WriteText(project / "content_source/waw_hud.json", py::JsonDumps(result, 2) + "\n");
        return Json{{"result", result}, {"stock_prepared", stock ? stock->PreparedJson() : Json()}};
    }
} // namespace w2b::pipeline
