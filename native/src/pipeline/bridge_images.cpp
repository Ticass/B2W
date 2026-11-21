#include "pipeline/bridge_images.h"

#include "core/asset_paths.h"
#include "core/parallel.h"
#include "pipeline/bridge_support.h"
#include "textures/iwd_recovery.h"
#include "textures/stage.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>

namespace w2b::pipeline
{
    namespace
    {
        using util::Json;
        using Color = std::array<unsigned char, 4>;
        using files::FromUtf8;

        struct Conversion
        {
            std::size_t index;
            fs::path source, destination;
            Json recovered;
        };

        struct DestinationOrder
        {
            bool operator()(const std::wstring& left, const std::wstring& right) const
            {
                return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            }
        };

        std::string ImageStem(std::string name, bool material)
        {
            if (material)
                for (const std::string_view prefix : {"waw_fx/", "waw_world/"})
                    if (name.starts_with(prefix))
                        name.erase(0, prefix.size());
            std::ranges::replace(name, '*', '_');
            return name;
        }

        std::map<std::string, std::string> WantedImages(const fs::path& project, const std::vector<std::string>& materialFiles,
                                                        const std::vector<std::string>& extras,
                                                        std::map<std::string, std::string>& semantics)
        {
            std::map<std::string, std::string> wanted;
            for (const auto& name : extras)
                wanted.try_emplace(name, ImageStem(name, false));
            for (const auto& file : materialFiles)
            {
                const auto material = mod::Json(project / FromUtf8(file));
                for (const auto& texture : material.value("textures", Json::array()))
                    if (const auto image = texture.find("image"); image != texture.end() && image->is_string())
                    {
                        const auto& name = image->get_ref<const std::string&>();
                        if (!name.empty() && !name.starts_with(','))
                        {
                            wanted.try_emplace(name, ImageStem(name, true));
                            // WaW keeps one semantic per image: the material texture slot that loads it
                            semantics.try_emplace(name, texture.value("semantic", std::string()));
                        }
                    }
            }
            wanted.insert_or_assign("lightmap0_secondary", "_lightmap0_secondary");
            wanted.insert_or_assign("reflection_probe0", "_reflection_probe0");
            wanted.insert_or_assign("$outdoor", "$outdoor");
            return wanted;
        }

        fs::path FindDds(const std::vector<fs::path>& roots, const std::string& stem)
        {
            const auto relative = FromUtf8(stem + ".dds");
            for (const auto& root : roots)
                if (fs::exists(root / relative))
                    return root / relative;
            return {};
        }

        Json RecoveredMetadata(const textures::RecoveredImage& image)
        {
            Json metadata{{"archive", mod::Str(image.archive)}, {"entry", image.entry}, {"iwi_format", image.iwiFormat}};
            if (image.decoderTables)
                metadata["decoder_tables"] = mod::Str(*image.decoderTables);
            return metadata;
        }

        // WaW sub_70F700: a streamed 2D image whose pixels fail to load is drawn with a built-in image
        // picked by the image's texture semantic (GfxImage+11): normal map (5) -> $identitynormalmap,
        // specular map (8) -> $black, anything else -> $white. Texels from sub_73C770's callers
        // (A8R8G8B8 written B,G,R,A; listed here as RGBA).
        std::pair<Color, std::string_view> AbsentImageStandIn(const std::string& semantic)
        {
            if (semantic == "normalMap")
                return {{128, 128, 255, 128}, "$identitynormalmap"};
            if (semantic == "specularMap")
                return {{0, 0, 0, 255}, "$black"};
            return {{255, 255, 255, 255}, "$white"};
        }

        // Images the mod's own mod.ff lists (each zone extraction records its fastfile). WaW loads mod.ff and
        // its delayed images (sub_48D630, end of every fastfile load) before the built-in images sub_70F700
        // hands out have pixels: measured in Kill House, mod.ff's missing images keep a NULL texture (the
        // Double Tap _on layer, an FX smoke image) while killhouse.ff's get $white/$black/$identitynormalmap.
        std::set<std::string> ModZoneImages(const std::vector<fs::path>& imageRoots)
        {
            std::set<std::string> names;
            for (const auto& root : imageRoots)
            {
                const auto extraction = root.parent_path() / "extraction.json";
                if (!fs::is_regular_file(extraction))
                    continue;
                const auto data = mod::Json(extraction);
                const auto file = data.value("inputs", Json::object()).value("file", std::string());
                if (mod::Lower(files::Utf8(FromUtf8(file).filename())) != "mod.ff")
                    continue;
                for (const auto& name : data.value("index", Json::object()).value("image", Json::array()))
                    if (name.is_string())
                        names.insert(ImageStem(name.get<std::string>(), false));
            }
            return names;
        }

        void Fill(std::string& pixels, const Color& rgba, unsigned count)
        {
            const std::string_view value(reinterpret_cast<const char*>(rgba.data()), rgba.size());
            for (unsigned i = 0; i < count; ++i)
                pixels += value;
        }

        Json WriteSynthesized(const std::string& name, const fs::path& output, std::string_view reason, Json& warnings,
                              unsigned width, unsigned height, std::span<const Color> pages, bool cube = false)
        {
            std::string pixels;
            const auto countPerPage = width * height / static_cast<unsigned>(pages.size()) * (cube ? 6 : 1);
            for (const auto& color : pages)
                Fill(pixels, color, countPerPage);
            files::WriteBytes(output, textures::RgbaToIwi(width, height, pixels, cube).bytes);
            warnings.push_back("image " + name + ": synthesized, " + std::string(reason));
            return {{"name", name}, {"source", nullptr}, {"synthesized", reason}};
        }

        std::optional<Json> RequiredImage(const std::string& asset, const fs::path& dst, const BridgeImagesRequest& request, Json& warnings)
        {
            if (asset == "lightmap0_secondary")
            {
                constexpr Color magenta{255, 0, 255, 255};
                const auto pages = request.diagnosticLightmap ? std::array{magenta, magenta, magenta} :
                    std::array<Color, 3>{{{116, 164, 181, 154}, {132, 172, 191, 111}, {129, 129, 192, 147}}};
                return WriteSynthesized(asset, dst, "fallback flat lightmap, linked only when the map has no WaW lightmap pages "
                                        "(BSP/lightmaps.json); pages = stock zm_nuked lightmap means", warnings, 4, 12, pages);
            }
            if (asset == "reflection_probe0")
            {
                const std::array<Color, 1> color{{{static_cast<unsigned char>(request.diagnosticProbes ? 255 : 0), 0, 0, 255}}};
                return WriteSynthesized(asset, dst, "black cube probe; the WaW probe is a solid red placeholder", warnings, 4, 4, color, true);
            }
            return std::nullopt;
        }

        void ConvertGroups(const std::vector<std::vector<Conversion>>& groups, const std::vector<std::string>& names,
                           std::vector<Json>& images, std::vector<bool>& written, Json& errors, unsigned workers)
        {
            std::vector<std::string> failures(names.size());
            parallel::For(groups.size(), [&](std::size_t group)
            {
                for (const auto& task : groups[group])
                    try
                    {
                        const auto header = textures::ConvertFile(task.source, task.destination);
                        Json image{{"name", names[task.index]}, {"source", mod::Str(task.source)}};
                        image.update(textures::HeaderJson(header));
                        if (!task.recovered.is_null())
                            image["wavelet"] = task.recovered;
                        images[task.index] = std::move(image);
                    }
                    catch (const textures::TextureError& error)
                    {
                        failures[task.index] = "image " + names[task.index] + ": " + error.what();
                    }
            }, workers);

            // Earlier assets own the first occurrence of a destination group.
            for (const auto& group : groups)
                for (const auto& task : group)
                    if (!failures[task.index].empty())
                        errors.push_back(failures[task.index]);
                    else
                        written[task.index] = true;
        }
    }

    BridgeImages StageBridgeImages(const BridgeImagesRequest& request, textures::IwdRecovery& recovery)
    {
        const auto& project = request.projectRoot;
        const auto& imageRoots = request.imageRoots;
        std::map<std::string, std::string> semantics;
        const auto wanted = WantedImages(project, request.materialFiles, request.extraImages, semantics);
        const auto modZoneImages = ModZoneImages(imageRoots);
        const auto& equivalents = request.nativeImages;
        auto knownRecovered = request.knownRecovered;
        std::optional<std::unordered_set<std::string>> iwdStems;
        auto warnings = Json::array(), errors = Json::array(), recovered = Json::object();
        std::vector<std::string> names;
        std::vector<Json> images(wanted.size());
        std::vector<bool> written(wanted.size());
        std::vector<std::vector<Conversion>> groups;
        std::map<std::wstring, std::size_t, DestinationOrder> byDestination;
        for (const auto& [asset, stem] : wanted)
        {
            const auto index = names.size();
            names.push_back(asset);
            const auto dst = project / assets::ImagePath(asset, ".iwi");
            if (equivalents.contains(asset))
            {
                if (!mod::IsFile(dst))
                    return {{"stage_error", "staged BO2 equivalent image " + asset + " disappeared"}};
                written[index] = true;
                continue;
            }
            if (auto image = RequiredImage(asset, dst, request, warnings))
            {
                images[index] = std::move(*image);
                written[index] = true;
                continue;
            }
            textures::RecoveredImage source;
            try
            {
                source = recovery.Resolve(stem, FindDds(imageRoots, stem));
            }
            catch (const textures::TextureError& error)
            {
                errors.push_back("image " + asset + ": " + error.what());
                continue;
            }
            catch (const fs::filesystem_error& error)
            {
                errors.push_back("image " + asset + ": " + error.what());
                continue;
            }
            if (source.path.empty())
            {
                const auto semantic = semantics.contains(asset) ? semantics.at(asset) : std::string();
                auto [texel, standIn] = AbsentImageStandIn(semantic);
                auto reason = "no pixel data in any zone or IWD; WaW draws " + std::string(standIn) + " for a missing " +
                              (semantic.empty() ? std::string("image") : semantic) + " (sub_70F700)";
                if (modZoneImages.contains(stem))
                {
                    texel = {0, 0, 0, 255};
                    reason = "no pixel data in any zone or IWD; the mod's mod.ff loads it before WaW's built-in images have "
                             "pixels, so sub_70F700 finds no stand-in and WaW samples a NULL texture (black)";
                }
                bool absent = stem.starts_with("~$white-rgb&");
                if (!absent && request.iwdDirs)
                {
                    if (!iwdStems)
                        iwdStems = recovery.ImageStems(*request.iwdDirs);
                    absent = !iwdStems->contains(mod::Lower(stem));
                }
                if (absent)
                    images[index] = WriteSynthesized(asset, dst, reason, warnings, 4, 4, std::array<Color, 1>{texel});
                written[index] = !images[index].is_null();
                if (!written[index])
                    errors.push_back("image " + asset + ": source " + stem + ".dds missing from every image root");
                continue;
            }
            if (!source.archive.empty())
                knownRecovered[stem] = recovered[stem] = RecoveredMetadata(source);
            const auto [key, first] = byDestination.try_emplace(fs::weakly_canonical(dst).wstring(), groups.size());
            if (first)
                groups.emplace_back();
            groups[key->second].push_back({index, std::move(source.path), dst, knownRecovered.value(stem, Json())});
        }
        ConvertGroups(groups, names, images, written, errors, request.workers);
        BridgeImages staged{{}, Json::array(), warnings.get<std::vector<std::string>>(), errors.get<std::vector<std::string>>(), std::move(recovered)};
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (written[index])
                staged.written.push_back(names[index]);
            if (!images[index].is_null())
                staged.images.push_back(std::move(images[index]));
        }
        return staged;
    }
}
