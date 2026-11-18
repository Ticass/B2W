#include "pipeline/menu_artwork.h"

#include "core/files.h"
#include "core/process.h"
#include "pipeline/menu_media.h"
#include "pipeline/menu_pixels.h"
#include "pipeline/menu_runtime.h"
#include "textures/block_compress.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace w2b::pipeline
{
    namespace
    {
        using launcher::Image;
        using py::Json;
        namespace fs = std::filesystem;

        bool SameCanvas(const Image& a, const Image& b)
        {
            return a.width == b.width && a.height == b.height;
        }

        int Round(double value)
        {
            return static_cast<int>(std::nearbyint(value));
        }

        Image PreviewFrame(const Image& image)
        {
            constexpr double aspect = 294.0 / 182;
            if (static_cast<double>(image.width) / image.height > aspect)
            {
                const auto keep = Round(image.height * aspect);
                return menu::Crop(image, (image.width - keep) / 2, 0, keep, image.height);
            }
            const auto keep = Round(image.width / aspect);
            return menu::Crop(image, 0, (image.height - keep) / 2, image.width, keep);
        }

        constexpr int GridWidth = 160, GridHeight = 90, BoxWidth = 50, BoxHeight = 25;

        std::pair<int, int> DensestBox(const Image& blit)
        {
            const auto alpha = menu::ResizeAlphaBox(blit, GridWidth, GridHeight);
            constexpr auto stride = GridWidth + 1;
            std::array<int, (GridHeight + 1) * stride> sum{};
            for (int y = 1; y <= GridHeight; ++y)
            {
                int row = 0;
                for (int x = 1; x <= GridWidth; ++x)
                {
                    row += alpha[(y - 1) * GridWidth + x - 1];
                    sum[y * stride + x] = row + sum[(y - 1) * stride + x];
                }
            }
            int best = -1;
            std::pair<int, int> point;
            for (int y = 0; y <= GridHeight - BoxHeight; ++y)
                for (int x = 0; x <= GridWidth - BoxWidth; ++x)
                {
                    const auto top = y * stride;
                    const auto bottom = (y + BoxHeight) * stride;
                    const int total = sum[bottom + x + BoxWidth] - sum[bottom + x] -
                                      sum[top + x + BoxWidth] + sum[top + x];
                    if (total > best)
                    {
                        best = total;
                        point = {x, y};
                    }
                }
            return point;
        }

        std::pair<int, int> StartPoint(const Image& blit, std::string_view input)
        {
            if (input.empty())
                return DensestBox(blit);
            const auto values = py::Split(input, ",");
            double x = 0, y = 0;
            if (values.size() != 2 || !py::ParseFloat(values[0], x) || !py::ParseFloat(values[1], y) ||
                !std::isfinite(x) || !std::isfinite(y))
                throw std::invalid_argument("Start location must be a pair of pixel coordinates.");
            return {Round(std::clamp(x / blit.width * GridWidth - BoxWidth / 2.0, 0.0, double(GridWidth - BoxWidth))),
                    Round(std::clamp(y / blit.height * GridHeight - BoxHeight / 2.0, 0.0, double(GridHeight - BoxHeight)))};
        }

        Image Location(const Image& blit, std::string_view coordinate, Json& metadata)
        {
            const auto [x, y] = StartPoint(blit, coordinate);
            metadata["start_location"] = {Round((x + BoxWidth / 2.0) / GridWidth * 1280 - 640),
                                          Round((y + BoxHeight / 2.0) / GridHeight * 720 - 360)};
            const auto left = Round(double(x) / GridWidth * blit.width);
            const auto top = Round(double(y) / GridHeight * blit.height);
            const auto right = Round(double(x + BoxWidth) / GridWidth * blit.width);
            const auto bottom = Round(double(y + BoxHeight) / GridHeight * blit.height);
            return menu::Resize(menu::Crop(blit, left, top, right - left, bottom - top), 512, 256);
        }

        Json Material(const std::string& image)
        {
            static const auto base = Json::parse(R"json({
                "_game":"t6","_type":"material","_version":1,"cameraRegion":"none",
                "constants":[],"contents":1,"gameFlags":[],"layeredSurfaceTypes":536870912,"sortKey":40,
                "stateBits":[{"alphaTest":"gt0","blendOpAlpha":"add","blendOpRgb":"add",
                    "colorWriteAlpha":true,"colorWriteRgb":true,"cullFace":"back","depthTest":"disabled",
                    "depthWrite":false,"dstBlendAlpha":"one","dstBlendRgb":"invsrcalpha",
                    "polygonOffset":"offset0","polymodeLine":false,"srcBlendAlpha":"invdestalpha","srcBlendRgb":"srcalpha"}],
                "stateBitsEntry":[],"stateFlags":0,"surfaceFlags":0,"surfaceTypeBits":0,
                "techniqueSet":"trivial_9z33feqw","textureAtlas":{"columns":1,"rows":1},
                "textures":[{"image":"","isMatureContent":false,"name":"colorMap",
                    "samplerState":{"clampU":true,"clampV":true,"clampW":true,"filter":"linear","mipMap":"disabled"},
                    "semantic":"2D"}]
            })json");
            auto result = base;
            auto values = std::vector<int>(36, -1);
            values[2] = 0;
            result["stateBitsEntry"] = std::move(values);
            result["textures"][0]["image"] = image;
            return result;
        }

        class ArtworkAssets
        {
        public:
            ArtworkAssets(const fs::path& root, const std::string& project)
                : root_(root), prefix_("menu_" + project + "_"), loading_("loadscreen_" + project + "_") {}

            void Texture(std::string_view role, const Image& image)
            {
                const bool alpha = role == "blit" || role == "icon" || image.MinAlpha() != 255;
                const auto packed = textures::EncodeMenuTexture(image.rgba, static_cast<unsigned>(image.width),
                    static_cast<unsigned>(image.height), alpha ? textures::Format::dxt5 : textures::Format::dxt1);
                WriteTexture(role, packed);
            }

            void WriteTexture(std::string_view role, const textures::IwiImage& image)
            {
                const auto name = "images/" + prefix_ + std::string(role) + ".iwi";
                files::WriteBytes(root_ / files::FromUtf8(name), image.bytes);
                names.push_back(name);
                details[role] = {{"format", image.header.format == textures::Format::dxt5 ? "DXT5" : "DXT1"},
                                 {"bytes", image.bytes.size()}, {"width", image.header.width}, {"height", image.header.height}};
            }

            void Bind(std::string_view variant, std::string_view role, bool loading = false)
            {
                const auto name = "materials/" + (loading ? loading_ : prefix_) + std::string(variant) + ".json";
                files::WriteText(root_ / files::FromUtf8(name), py::JsonDumps(Material(prefix_ + std::string(role)), 2));
                names.push_back(name);
            }

            Json names = Json::array();
            Json details = Json::object();

        private:
            const fs::path& root_;
            std::string prefix_, loading_;
        };

        Image OpenImage(const std::string& name, const fs::path& decoder)
        {
            const auto path = files::FromUtf8(name);
            try
            {
                return launcher::DecodeImage(path);
            }
            catch (const std::runtime_error&)
            {
                // WebP is exposed by the launcher. Its WIC codec depends on
                // the Windows installation, so use the packaged decoder when absent.
                if (launcher::Lower(files::Utf8(path.extension())) != ".webp" || !fs::is_regular_file(path))
                    throw;
                const auto program = FindMenuDecoder(decoder);
                if (program.empty())
                    throw;
                const TemporaryMenuFile scratch;
                const auto result = process::RunMerged({files::Utf8(program), "-v", "error", "-nostdin", "-y", "-i",
                    files::Utf8(fs::absolute(path)), "-frames:v", "1", "-pix_fmt", "rgba", "-c:v", "png",
                    "-f", "image2", "-update", "1", files::Utf8(scratch.Path())});
                if (result.exitCode)
                    throw;
                return launcher::DecodeImage(scratch.Path());
            }
        }
    }

    Json MenuMaterial(const std::string& image)
    {
        return Material(image);
    }

    std::optional<MenuArtwork> LoadMenuArtwork(const launcher::Settings& settings, const fs::path& decoder)
    {
        const auto count = !settings.menu_blit.empty() + !settings.menu_large.empty() + !settings.menu_blur.empty();
        if (count == 0)
            return std::nullopt;
        if (count != 3)
            throw std::invalid_argument("Choose all three images: Blit, Large, and Blur.");
        MenuArtwork art{OpenImage(settings.menu_blit, decoder), OpenImage(settings.menu_large, decoder),
                        OpenImage(settings.menu_blur, decoder), false, {}};
        if (art.blit.MinAlpha() == 255)
            throw std::invalid_argument("Blit needs a transparent background. Choose a PNG or TGA with alpha transparency.");
        const auto equal = SameCanvas(art.blit, art.large) && SameCanvas(art.large, art.blur);
        const auto stock = art.blit.width == 512 && art.blit.height == 256 &&
                           art.large.width == 2048 && art.large.height == 2048 && SameCanvas(art.large, art.blur);
        art.aligned = !stock;
        if (!equal && !stock)
            art.warning = "Layer canvas sizes differ. They will be resized to the same canvas, but objects may not line up "
                          "and the result may look distorted. You can still preview and build.";
        return art;
    }

    void StageMenuArtwork(const launcher::Settings& settings, const fs::path& root, const fs::path& compat,
                          const fs::path& decoder, MenuArtwork art, Json& metadata)
    {
        ArtworkAssets files(root, settings.project);
        Image complete = art.large;
        std::optional<textures::IwiImage> packedComplete;
        if (art.aligned)
        {
            const auto alignedBlit = menu::Resize(art.blit, art.large.width, art.large.height);
            menu::Composite(complete, alignedBlit);
            auto focused = menu::Resize(art.blur, art.large.width, art.large.height);
            menu::Composite(focused, alignedBlit);
            const auto large = menu::Resize(complete, 2048, 2048);
            packedComplete = textures::EncodeMenuTexture(large.rgba, 2048, 2048,
                large.MinAlpha() < 255 ? textures::Format::dxt5 : textures::Format::dxt1);
            files.WriteTexture("large", *packedComplete);
            files.Texture("blur", menu::Resize(focused, 2048, 2048));
            files.Texture("blit", menu::Resize(art.blit, 2048, 2048));
            files.Texture("location", Location(art.blit, settings.menu_location_point, metadata));
        }
        else
        {
            menu::Composite(complete, menu::Resize(art.blit, 320, 284), 864, 882);
            files.Texture("blit", art.blit);
            files.Texture("large", art.large);
            files.Texture("blur", art.blur);
        }
        if (packedComplete)
            files.WriteTexture("complete", *packedComplete);
        else
            files.Texture("complete", menu::Resize(complete, 2048, 2048));
        files.Texture("icon", menu::Resize(settings.menu_icon.empty() ? complete : OpenImage(settings.menu_icon, decoder), 256, 256));
        const auto loading = settings.menu_loading.empty() ? "complete" : "loading";
        if (!settings.menu_loading.empty())
            files.Texture("loading", menu::Resize(OpenImage(settings.menu_loading, decoder), 2048, 2048));
        files.Texture("preview", menu::Resize(PreviewFrame(settings.menu_preview.empty() ? complete :
                                                             OpenImage(settings.menu_preview, decoder)), 2048, 2048));
        files.Bind("map", "large");
        files.Bind("map_blur", "blur");
        files.Bind("blit", "blit");
        files.Bind("icon", "icon");
        files.Bind("zclassic_default", "preview");
        files.Bind("zclassic_default", loading, true);
        files.Bind("zclassic_", loading, true);
        if (art.aligned)
            files.Bind("loc", "location");
        metadata["icon"] = "menu_" + settings.project + "_icon";
        metadata["blit"] = "menu_" + settings.project + "_blit";
        metadata["texture_report"] = std::move(files.details);
        metadata.update(StageMenuRuntime(launcher::Path(settings.bo2), root, settings.project, compat, art.aligned));
        metadata["desktop_files"] = std::move(files.names);
    }
}
