// WaW's engine-owned HUD assets (see hud_stage.h).
#include "hud_stage.h"

#include "assets/resolver.h"
#include "core/unicode.h"
#include "csvtable.h"
#include "extract/stockwaw.h"
#include "menu_artwork.h"
#include "modutil.h"
#include "textures/block_compress.h"
#include "textures/iwd_recovery.h"
#include "textures/iwi.h"
#include "textures/pillow_decode.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::IsFile;
    using mod::StartsWith;
    using util::Json;

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
            SourceFinder(std::vector<fs::path> roots, extract::StockWaw* stock, textures::IwdRecovery* recovery = nullptr)
                : roots_(std::move(roots)), stock_(stock), recovery_(recovery)
            {
            }

            Json recovered = Json::object();

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
                        const auto found = Find("image", image.get<std::string>());
                        if (!recovery_)
                            return found;
                        const auto resolved = recovery_->Resolve(image.get<std::string>(), found ? *found : fs::path());
                        if (!resolved.archive.empty())
                        {
                            Json metadata{{"archive", files::Utf8(resolved.archive)}, {"entry", resolved.entry}, {"iwi_format", resolved.iwiFormat}};
                            if (resolved.decoderTables)
                                metadata["decoder_tables"] = files::Utf8(*resolved.decoderTables);
                            recovered[image.get<std::string>()] = metadata;
                        }
                        return resolved.path.empty() ? std::nullopt : std::optional(resolved.path);
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

            // the stock WaW copy of an asset, whatever the map ships
            std::optional<fs::path> Stock(const std::string& kind, const std::string& name, const fs::path& relative)
            {
                if (!stock_)
                    return std::nullopt;
                const auto hit = stock_->RootFor(kind, name);
                return hit && IsFile(hit->second / relative) ? std::optional(hit->second / relative) : std::nullopt;
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
            textures::IwdRecovery* recovery_;
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
                return util::FloatRepr(v.get<double>());
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
            Stager(fs::path project, std::string folder) : project_(std::move(project)), folder_(std::move(folder))
            {
            }

            // write_sprite: a lossless RGBA 2D material. T6 refuses to load an image
            // of 16 MiB or more ("Needed to allocate at least 16.0 MB to load
            // images"), so one that big (a 2048 x 2048 WaW loadscreen) is block-
            // compressed instead: DXT1 when fully opaque, else DXT5.
            std::string Sprite(const std::string& shortName, const textures::RgbaImage& image)
            {
                const auto name = folder_ + "/" + shortName;
                auto file = name;
                std::replace(file.begin(), file.end(), '*', '_');
                const auto target = project_ / "content_source" / "images" / files::FromUtf8(file + ".iwi");
                constexpr std::size_t imageLimit = 16u * 1024 * 1024;
                if (image.pixels.size() + 64 >= imageLimit && image.width % 4 == 0 && image.height % 4 == 0)
                {
                    bool opaque = true;
                    for (std::size_t i = 3; opaque && i < image.pixels.size(); i += 4)
                        opaque = static_cast<unsigned char>(image.pixels[i]) == 255;
                    const std::span pixels(reinterpret_cast<const std::uint8_t*>(image.pixels.data()), image.pixels.size());
                    files::WriteBytes(target, textures::EncodeMenuTexture(pixels, image.width, image.height,
                                                                          opaque ? textures::Format::dxt1 : textures::Format::dxt5).bytes);
                }
                else
                    files::WriteBytes(target, textures::RgbaToIwi(image.width, image.height, image.pixels).bytes);
                const auto material = project_ / "content_source" / assets::SourcePath({"material", name});
                files::WriteText(material, util::JsonDumpsDefault(MenuMaterial(name)));
                materials.push_back(name);
                return name;
            }

            std::vector<std::string> materials;

        private:
            fs::path project_;
            std::string folder_;
        };

        // The atlas rectangle (x, y, rw, rh in texels) bilinearly resampled to
        // w x h at the top left of a cleared cw x ch image.
        textures::RgbaImage ResampleGlyph(const textures::RgbaImage& atlas, const double x, const double y, const double rw, const double rh,
                                          const unsigned w, const unsigned h, const unsigned cw, const unsigned ch)
        {
            textures::RgbaImage out(cw, ch);
            const auto texel = [&](long long tx, long long ty, unsigned channel) -> double
            {
                if (tx < 0 || ty < 0 || tx >= atlas.width || ty >= atlas.height)
                    return 0.0;
                return static_cast<unsigned char>(atlas.pixels[(static_cast<std::size_t>(ty) * atlas.width + static_cast<std::size_t>(tx)) * 4 + channel]);
            };
            for (unsigned row = 0; row < h && row < ch; ++row)
                for (unsigned col = 0; col < w && col < cw; ++col)
                {
                    const auto sx = x + (col + 0.5) * rw / w - 0.5, sy = y + (row + 0.5) * rh / h - 0.5;
                    const auto fx = std::floor(sx), fy = std::floor(sy);
                    const auto ax = sx - fx, ay = sy - fy;
                    const auto ix = static_cast<long long>(fx), iy = static_cast<long long>(fy);
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const auto top = texel(ix, iy, channel) * (1 - ax) + texel(ix + 1, iy, channel) * ax;
                        const auto bottom = texel(ix, iy + 1, channel) * (1 - ax) + texel(ix + 1, iy + 1, channel) * ax;
                        out.pixels[(static_cast<std::size_t>(row) * cw + col) * 4 + channel] =
                            static_cast<char>(static_cast<unsigned char>(std::clamp(std::lround(top * (1 - ay) + bottom * ay), 0L, 255L)));
                    }
                }
            return out;
        }

        // whether the atlas rectangles of a font have the proportions its glyphs are drawn with
        bool TableFitsAtlas(const Json& font, const textures::RgbaImage& atlas)
        {
            std::size_t fits = 0, stretched = 0;
            for (const auto& glyph : font.at("glyphs"))
            {
                const auto w = glyph.at("pixelWidth").get<double>(), h = glyph.at("pixelHeight").get<double>();
                if (w < 4 || h < 4)
                    continue;
                const auto x = (glyph.at("s1").get<double>() - glyph.at("s0").get<double>()) * atlas.width / w;
                const auto y = (glyph.at("t1").get<double>() - glyph.at("t0").get<double>()) * atlas.height / h;
                (x > y * 1.5 || y > x * 1.5 ? stretched : fits)++;
            }
            return stretched <= fits;
        }

        // whether two glyph tables are one font: stock compiles differ in a few small glyphs (Hotel V2: 8 of 191)
        bool SameTable(const Json& a, const Json& b)
        {
            const auto &x = a.at("glyphs"), &y = b.at("glyphs");
            if (x.size() != y.size())
                return false;
            std::size_t equal = 0;
            for (std::size_t i = 0; i < x.size(); ++i)
                equal += x[i] == y[i];
            return equal * 10 >= x.size() * 9;
        }

        // font_glyphs: glyph table rows of a source font
        std::vector<std::vector<std::string>> FontGlyphs(SourceFinder& source, const std::string& fontName, const std::string& role, Stager& stager,
                                                         const std::set<char32_t>* chars = nullptr)
        {
            const auto path = source.Font(fontName);
            if (!path)
                throw Missing(fontName);
            auto font = mod::Json(*path);
            const auto materialName = font.at("material").get<std::string>();
            const auto material = source.Find("material", materialName);
            if (!material)
                throw Missing(materialName);
            const auto imageName = mod::Json(*material).at("textures").at(0).at("image").get<std::string>();
            const auto atlasPath = source.Find("image", imageName);
            if (!atlasPath)
                throw Missing(imageName);
            std::vector<std::vector<std::string>> rows;
            auto atlas = textures::DecodeDdsPillow(mod::Bytes(*atlasPath));
            // A map can replace the shared atlas image and still carry the stock table for one of its fonts (Hotel V2:
            // objectiveFont on a 512x1024 gamefonts_pc). The stock table only fits the stock atlas.
            if (!TableFitsAtlas(font, atlas))
                if (const auto stockFont = source.Stock("font", fontName, files::FromUtf8(fontName + ".json")))
                    if (auto stock = mod::Json(*stockFont); SameTable(stock, font))
                        if (const auto stockAtlas = source.Stock("image", imageName, assets::SourcePath({"image", imageName})))
                        {
                            font = std::move(stock);
                            atlas = textures::DecodeDdsPillow(mod::Bytes(*stockAtlas));
                            mod::Print("WARNING: " + fontName + ": its glyph table is stock WaW's and does not fit the map's " + imageName +
                                       "; drawn from the stock font and atlas");
                        }
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
                if (chars && !chars->count(code))
                    continue;
                const auto w = glyph.at("pixelWidth").get<long long>(), h = glyph.at("pixelHeight").get<long long>();
                std::string name;
                long long pw = 0, ph = 0;
                if (w && h)
                {
                    const auto s0 = glyph.at("s0").get<double>(), t0 = glyph.at("t0").get<double>();
                    const auto x = static_cast<long long>(std::nearbyint(s0 * atlas.width));
                    const auto y = static_cast<long long>(std::nearbyint(t0 * atlas.height));
                    pw = std::max(4LL, 1LL << BitLength(w - 1));
                    ph = std::max(4LL, 1LL << BitLength(h - 1));
                    // T4 draws the s0..s1 x t0..t1 atlas rectangle on a pixelWidth x
                    // pixelHeight quad. Stock atlases hold one texel per pixel; a map
                    // font may be denser (Clinic of Evil: about 4x), so the sprite keeps
                    // that many texels per pixel and the rectangle is resampled into it.
                    const auto rw = (glyph.at("s1").get<double>() - s0) * atlas.width;
                    const auto rh = (glyph.at("t1").get<double>() - t0) * atlas.height;
                    const auto scale = std::max(1LL, std::llround(rh / static_cast<double>(h)));
                    const auto gw = w * scale, gh = h * scale;
                    textures::RgbaImage cell;
                    if (scale == 1 || (std::llround(rw) == gw && std::llround(rh) == gh))
                        cell = atlas.Cell(static_cast<unsigned>(x), static_cast<unsigned>(y), static_cast<unsigned>(gw), static_cast<unsigned>(gh),
                                          static_cast<unsigned>(pw * scale), static_cast<unsigned>(ph * scale));
                    else
                        cell = ResampleGlyph(atlas, s0 * atlas.width, t0 * atlas.height, rw, rh, static_cast<unsigned>(gw), static_cast<unsigned>(gh),
                                             static_cast<unsigned>(pw * scale), static_cast<unsigned>(ph * scale));
                    name = stager.Sprite(role + "_" + Hex4(static_cast<unsigned>(code)), cell);
                }
                rows.push_back({role + ":" + std::to_string(static_cast<unsigned long>(code)), name, Cell(glyph.at("x0")), Cell(glyph.at("y0")),
                                Cell(glyph.at("dx")), std::to_string(pw), std::to_string(ph), Cell(font.at("pixelHeight"))});
            }
            return rows;
        }
    } // namespace

    Json StageWawHud(const fs::path& project, const std::vector<fs::path>& roots, extract::StockWaw* stock)
    {
        SourceFinder source(roots, stock);
        Stager stager(project, "waw_hud");
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
        files::WriteText(project / "content_source/waw_hud.json", util::JsonDumps(result, 2) + "\n");
        return result;
    }

    namespace
    {
        // wawmenu.FONT_ROLES / FONT_FILES
        const std::map<long long, std::vector<std::string>> FONT_ROLES{
            {0, {"small", "extrabig", "big", "normal"}}, {1, {"normal"}}, {3, {"small"}}, {4, {"small", "bold", "normal"}}, {5, {"console"}}, {6, {"objective"}}};
        const std::map<std::string, std::string> FONT_FILES{{"small", "fonts/smallFont"},   {"normal", "fonts/normalFont"}, {"big", "fonts/bigFont"},
                                                            {"extrabig", "fonts/extraBigFont"}, {"bold", "fonts/boldFont"},   {"console", "fonts/consoleFont"},
                                                            {"objective", "fonts/objectiveFont"}};

        const std::vector<std::string>& FontRoles(const long long font)
        {
            const auto it = FONT_ROLES.find(font);
            return it == FONT_ROLES.end() ? FONT_ROLES.at(0) : it->second;
        }

        // wawmenu.glyph_sets: the characters each font role must carry
        std::map<std::string, std::set<char32_t>> GlyphSets(const Json& required, const std::map<std::string, std::string>& localized)
        {
            std::map<std::string, std::set<char32_t>> out;
            for (const auto& [key, enums] : required.at("strings").items())
            {
                auto value = localized.contains(key) ? localized.at(key) : key;
                std::string stripped;
                for (size_t i = 0; i < value.size(); i++)
                {
                    if (value[i] == '^' && i + 1 < value.size() && value[i + 1] >= '0' && value[i + 1] <= '9')
                    {
                        i++;
                        continue;
                    }
                    stripped += value[i];
                }
                const auto points = unicode::Decode(stripped);
                for (const auto& font : enums)
                    for (const auto& role : FontRoles(font.get<long long>()))
                        out[role].insert(points.begin(), points.end());
            }
            for (const auto& [font, chars] : required.at("literal_text").items())
            {
                const auto points = unicode::Decode(chars.get<std::string>());
                for (const auto& role : FontRoles(std::stoll(font)))
                    out[role].insert(points.begin(), points.end());
            }
            for (auto& [_, chars] : out)
            {
                chars.insert(U'?'); // an unknown glyph draws as '?'
                chars.erase(U'\n');
            }
            return out;
        }
    } // namespace

    MenuArt StageWawMenuArt(const fs::path& project, const std::vector<fs::path>& roots, extract::StockWaw* stock,
                            const std::map<std::string, std::string>& localized, textures::IwdRecovery& recovery)
    {
        const auto manifestPath = project / "content_source/waw_menu.json";
        auto manifest = mod::Json(manifestPath);
        SourceFinder source(roots, stock, &recovery);
        Stager stager(project, "waw_menu");
        std::vector<std::string> missing;
        for (const auto& item : manifest.at("materials"))
        {
            const auto name = item.get<std::string>();
            const auto path = source.MaterialImage(name);
            if (!path)
            {
                missing.push_back(name);
                continue;
            }
            stager.Sprite(name, textures::DecodeDdsPillow(mod::Bytes(*path)));
        }
        std::vector<std::vector<std::string>> rows;
        for (const auto& [role, chars] : GlyphSets(manifest, localized))
        {
            try
            {
                auto found = FontGlyphs(source, FONT_FILES.at(role), role, stager, &chars);
                rows.insert(rows.end(), found.begin(), found.end());
            }
            catch (const Missing& error)
            {
                missing.push_back(error.what());
            }
        }
        files::WriteText(project / "zm/waw_menu_glyphs.csv", CsvText(rows, "\n"));
        manifest["staged_materials"] = stager.materials;
        manifest["missing"] = missing;
        manifest["glyphs"] = rows.size();
        files::WriteText(manifestPath, util::JsonDumps(manifest, 2) + "\n");
        return {manifest.at("menus").size(), stager.materials.size(), rows.size(), std::move(missing), std::move(source.recovered)};
    }
} // namespace w2b::pipeline
