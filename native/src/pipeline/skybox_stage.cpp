// The map's own sky dome (see skybox_stage.h).
#include "skybox_stage.h"

#include "core/asset_paths.h"
#include "finish.h"
#include "materials/waw_material_json.h"
#include "modutil.h"
#include "shaders/runtime.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace w2b::pipeline
{
    using mod::IsFile;
    using mod::StartsWith;
    using util::Json;

    namespace
    {
        const std::string SKYBOX_MATERIAL = "mc/mtl_dlc0_zm_skybox_nuketown";

        struct StageError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        std::optional<fs::path> Find(const std::vector<fs::path>& roots, const fs::path& relative)
        {
            for (const auto& root : roots)
                if (IsFile(root / relative))
                    return root / relative;
            return std::nullopt;
        }

        // fxmaterials._main_state: the first state that draws normally
        const materials::WawStateBits* MainState(const materials::WawMaterial& material)
        {
            for (const auto& s : material.stateBits)
                if (!s.polymodeLine && s.depthTest != "disabled")
                    return &s;
            return nullptr;
        }

        void CopyFile(const fs::path& from, const fs::path& to)
        {
            fs::create_directories(to.parent_path());
            fs::copy_file(from, to, fs::copy_options::overwrite_existing);
            fs::last_write_time(to, fs::last_write_time(from));
        }

        // bridge_bone_bounds: Mod Tools vector objects as the bridge's arrays
        void BridgeBoneBounds(Json& model)
        {
            if (!model.contains("boneInfo"))
                return;
            for (auto& bone : model["boneInfo"])
                for (const char* bound : {"mins", "maxs", "offset"})
                    if (bone.contains(bound) && bone[bound].is_object())
                    {
                        Json values = Json::array();
                        for (const char* axis : {"x", "y", "z"})
                        {
                            if (!bone[bound].contains(axis))
                                throw StageError("xmodel bone " + (bone.contains("bone") ? bone["bone"].get<std::string>() : std::string("?")) + ": " + bound + " lacks " + axis + " coordinate");
                            values.push_back(bone[bound][axis]);
                        }
                        bone[bound] = values;
                    }
        }

        // T6 sky cubemaps (mc_skycubemaphdr) carry an HDR intensity in alpha;
        // WaW sky cubes are LDR with alpha 0. round(255 / 6) in Python: the
        // LDR colour comes out at about its WaW brightness with the stock
        // material's skyColorParms intensity of 6.
        constexpr unsigned char SKY_HDR_ALPHA = 42;
        const std::string SKYBOX_TEMPLATE = "skybox_dlc0_zm_nuketown";
        const std::string SKY_SOURCE_DIR = "sky_src";
        const std::string NO_SKY_IMAGE = "waw2bo2_no_sky";

        void PutU32(std::string& data, std::size_t offset, std::uint32_t value)
        {
            std::memcpy(data.data() + offset, &value, 4);
        }

        std::uint32_t GetU32(const std::string& data, std::size_t offset)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, data.data() + offset, 4);
            return value;
        }

        // repr() of a bytearray
        std::string BytearrayRepr(std::string_view bytes)
        {
            const bool doubleQuote = bytes.find('\'') != std::string_view::npos && bytes.find('"') == std::string_view::npos;
            const char quote = doubleQuote ? '"' : '\'';
            std::string out = std::string("bytearray(b") + quote;
            for (const unsigned char c : bytes)
            {
                if (c == '\\' || c == static_cast<unsigned char>(quote))
                    out += std::string("\\") + static_cast<char>(c);
                else if (c == '\t')
                    out += "\\t";
                else if (c == '\n')
                    out += "\\n";
                else if (c == '\r')
                    out += "\\r";
                else if (c < 0x20 || c >= 0x7f)
                {
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "\\x%02x", c);
                    out += hex;
                }
                else
                    out += static_cast<char>(c);
            }
            return out + quote + ")";
        }

        // black_cube_dds: an uncompressed A8R8G8B8 cube map (6 faces, one mip),
        // the layout SkyWithHdrAlpha writes for WaW sky cubes
        std::string BlackCubeDds(std::uint32_t size, unsigned char alpha)
        {
            std::string data(128, '\0');
            data.replace(0, 4, "DDS ");
            const std::uint32_t header[] = {124, 0x100F, size, size, size * 4, 0, 1};
            for (std::size_t i = 0; i < std::size(header); ++i)
                PutU32(data, 4 + 4 * i, header[i]);
            const std::uint32_t pixelFormat[] = {32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000};
            for (std::size_t i = 0; i < std::size(pixelFormat); ++i)
                PutU32(data, 76 + 4 * i, pixelFormat[i]);
            PutU32(data, 108, 0x1008);
            PutU32(data, 112, 0xFE00);
            std::string face;
            for (std::uint32_t i = 0; i < size * size; ++i)
                face += std::string{'\0', '\0', '\0', static_cast<char>(alpha)};
            for (int i = 0; i < 6; ++i)
                data += face;
            return data;
        }

        // _sky_with_hdr_alpha: the WaW sky cube with the T6 HDR intensity in
        // alpha (uncompressed 32-bit DDS only); returns the image name to use
        std::string SkyWithHdrAlpha(BridgeReport& report, const fs::path& projectRoot, const std::string& skyImage, const std::vector<fs::path>& imageRoots)
        {
            const auto source = Find(imageRoots, files::FromUtf8(skyImage + ".dds"));
            if (!source)
            {
                report.Fail("skybox: " + skyImage + ".dds not found in the image roots");
                return skyImage;
            }
            auto data = mod::Bytes(*source);
            const auto fourcc = data.substr(84, 4);
            const auto bits = GetU32(data, 88);
            if (fourcc != std::string(4, '\0') || bits != 32)
            {
                report.Warn("skybox: " + skyImage + " is " + BytearrayRepr(fourcc) + "/" + std::to_string(bits) +
                                             "bpp; its alpha is not rewritten, the T6 sky may render dark");
                return skyImage;
            }
            const auto alphaMask = GetU32(data, 104);
            const std::size_t shift = alphaMask == 0x000000FF ? 0 : 3;
            if (!alphaMask)
            {
                // X8R8G8B8 -> A8R8G8B8 (converted to an RGBA IWI, not 24-bit RGB)
                PutU32(data, 80, GetU32(data, 80) | 0x1);
                PutU32(data, 104, 0xFF000000);
            }
            for (auto i = 128 + shift; i < data.size(); i += 4)
                data[i] = static_cast<char>(SKY_HDR_ALPHA);
            const auto name = skyImage + "_t6sky";
            files::WriteBytes(projectRoot / SKY_SOURCE_DIR / files::FromUtf8(name + ".dds"), data);
            report.Warn("skybox: " + skyImage + " alpha set to " + std::to_string(SKY_HDR_ALPHA) +
                                         "/255 as the T6 HDR sky intensity (WaW sky cubes have none)");
            return name;
        }
    } // namespace

    Json SourceSky::ToJson() const
    {
        return {{"source_model", sourceModel}, {"status", "source_geometry_and_paired_shaders"}, {"materials", materials}};
    }

    SourceSkyStage StageSourceSkybox(const SourceSkyRequest& request)
    {
        const auto& project = request.project;
        const auto& projectRoot = request.projectRoot;
        const auto& roots = request.roots;
        const auto& sourceName = request.sourceName;
        const auto& nativeRoot = request.nativeRoot;
        const auto& bo2 = request.bo2Root;
        try
        {
            const fs::path modelRelative = fs::path("xmodel") / files::FromUtf8(sourceName + ".json");
            const auto sourceFile = Find(roots, modelRelative);
            if (!sourceFile)
                throw StageError("source sky model " + sourceName + " missing");
            auto model = mod::Json(*sourceFile);
            const auto donor = materials::ReadT6Material(bo2 / "raw/materials" / files::FromUtf8(SKYBOX_MATERIAL + ".json"));
            std::set<std::string> materialNames;
            fs::path sourceRoot;
            for (const auto& root : roots)
                if (root / modelRelative == *sourceFile)
                {
                    sourceRoot = root;
                    break;
                }
            for (const auto& lod : model.at("lods"))
            {
                const auto file = lod.at("file").get<std::string>();
                weapons::OutputName("model_file", file);
                // weapons.model_lod_source: the owning zone first
                std::vector<fs::path> order{sourceRoot};
                for (const auto& r : roots)
                    if (std::find(order.begin(), order.end(), r) == order.end())
                        order.push_back(r);
                const auto original = Find(order, files::FromUtf8(file));
                if (!original)
                    throw StageError("source sky model " + sourceName + ": geometry " + file + " missing from WaW assets");
                const auto mesh = mod::Json(*original);
                for (const auto& m : mesh.value("materials", Json::array()))
                    materialNames.insert(m.at("name").get<std::string>());
                const auto destination = projectRoot / files::FromUtf8(file);
                CopyFile(*original, destination);
                for (const auto& buffer : mesh.value("buffers", Json::array()))
                {
                    const auto uri = buffer.value("uri", std::string());
                    if (!uri.empty() && !StartsWith(uri, "data:"))
                        CopyFile(original->parent_path() / files::FromUtf8(uri), destination.parent_path() / files::FromUtf8(uri));
                }
            }
            std::vector<MaterialRecord> materials;
            for (const auto& name : materialNames)
            {
                const auto materialFile = Find(roots, assets::MaterialPath(name));
                if (!materialFile)
                    throw StageError("source sky material " + name + " missing");
                auto source = materials::FromOatJson(mod::Json(*materialFile));
                source.skyModel = true;
                auto target = donor;
                target.textures.clear();
                for (const auto& texture : source.textures)
                    target.textures.push_back(materials::ToT6(texture));
                target.constants.clear();
                for (const auto& constant : source.constants)
                {
                    materials::T6Constant out;
                    out.name = constant.name;
                    if (!constant.name)
                    {
                        out.nameFragment = constant.nameFragment;
                        out.nameHash = constant.nameHash;
                    }
                    for (std::size_t i = 0; i < 4; ++i)
                        out.literal[i] = constant.literal[i];
                    target.constants.push_back(std::move(out));
                }
                target.sortKey = source.sortKey;
                if (const auto* state = MainState(source))
                {
                    const auto wanted = materials::ToT6(*state);
                    for (auto& native : target.stateBits)
                        if (!native.polymodeLine)
                        {
                            materials::CopyBlendState(native, wanted);
                            materials::CopyRasterState(native, wanted);
                        }
                }
                shaders::FolderOutput output(projectRoot);
                const auto runtime = shaders::BindMaterial(source, target, roots, output, nativeRoot);
                bool paired = false;
                for (const auto& a : runtime.value("active", Json::array()))
                    if (mod::Truthy(a, "paired") && a.at("slot") == 2)
                        paired = true;
                if (!paired)
                {
                    std::vector<std::string> unsupported;
                    for (const auto& u : runtime.value("unsupported", Json::array()))
                        unsupported.push_back(u.get<std::string>());
                    throw StageError("source sky shader " + name + " unsupported: " + mod::ListRepr(unsupported));
                }
                const auto relative = assets::MaterialPath(name);
                files::WriteText(projectRoot / relative, materials::T6MaterialText(target));
                MaterialRecord record;
                record.name = name;
                record.file = mod::Generic(relative);
                record.t6Techset = target.techniqueSet;
                record.shaderRuntime = runtime;
                materials.push_back(std::move(record));
            }
            model["_game"] = "t6";
            BridgeBoneBounds(model);
            model["flags"] = 0x200000;
            model["lightingOriginOffset"] = Json{{"x", 0.0}, {"y", 0.0}, {"z", 0.5}};
            model["lightingOriginRange"] = 0.5;
            model.erase("physPreset");
            files::WriteText(projectRoot / "xmodel" / files::FromUtf8("skybox_" + project + ".json"), util::JsonDumps(model, 2) + "\n");
            return {std::move(materials), {sourceName, {materialNames.begin(), materialNames.end()}}};
        }
        catch (const StageError& error)
        {
            throw StepError("StageError", error.what());
        }
    }

    TemplateSky StageTemplateSkybox(BridgeReport& report, const std::string& project, const fs::path& projectRoot, const std::optional<fs::path>& bo2Root,
                                    std::optional<std::string> skyImage, const std::vector<fs::path>& imageRoots)
    {
        if (!bo2Root)
        {
            report.Fail("skybox: needs --bo2");
            return {};
        }
        const bool synthesized = !skyImage;
        if (synthesized)
        {
            // An enclosed map with no sky surface and no skybox model: WaW
            // draws nothing there (black). Keep T6's dome behind everything, black.
            skyImage = NO_SKY_IMAGE;
            files::WriteBytes(projectRoot / SKY_SOURCE_DIR / files::FromUtf8(*skyImage + ".dds"), BlackCubeDds(4, SKY_HDR_ALPHA));
            report.Warn("SKY_SYNTHESIZED the WaW map has no sky surface or skybox model; the T6 sky dome is black, as WaW draws it");
        }
        const auto raw = *bo2Root / "raw";
        const auto sourceModel = raw / "xmodel" / files::FromUtf8(SKYBOX_TEMPLATE + ".json");
        const auto sourceMaterial = raw / assets::MaterialPath(SKYBOX_MATERIAL);
        if (!IsFile(sourceModel) || !IsFile(sourceMaterial))
        {
            report.Fail("skybox: " + mod::Str(sourceModel) + " or " + mod::Str(sourceMaterial) + " missing (run the mod tools All2Raw)");
            return {};
        }
        auto model = mod::Json(sourceModel);
        try
        {
            BridgeBoneBounds(model);
        }
        catch (const StageError& error)
        {
            throw StepError("StageError", error.what());
        }
        for (const auto& lod : model.value("lods", Json::array()))
        {
            const auto file = files::FromUtf8(lod.at("file").get<std::string>());
            if (!IsFile(raw / file))
            {
                report.Fail("skybox: " + mod::Str(raw / file) + " missing");
                return {};
            }
            CopyFile(raw / file, projectRoot / file);
        }
        files::WriteText(projectRoot / "xmodel" / files::FromUtf8("skybox_" + project + ".json"), util::JsonDumps(model, 4) + "\n");
        if (!synthesized)
            skyImage = SkyWithHdrAlpha(report, projectRoot, *skyImage, imageRoots);
        auto material = mod::Json(sourceMaterial);
        for (auto& texture : material["textures"])
            if (texture.value("name", std::string()) == "colorMap")
                texture["image"] = *skyImage;
        files::WriteText(projectRoot / assets::MaterialPath(SKYBOX_MATERIAL), util::JsonDumps(material, 2) + "\n");
        report.Warn("skybox: BO2 " + SKYBOX_TEMPLATE + " dome with the WaW sky cube " + *skyImage);
        TemplateSky sky{std::nullopt, skyImage};
        if (material.contains("techniqueSet") && material["techniqueSet"].is_string())
            sky.techset = material["techniqueSet"].get<std::string>();
        return sky;
    }
} // namespace w2b::pipeline
