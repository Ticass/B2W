// The weapons' materials and images (see weapon_visuals.h).
#include "weapon_visuals.h"

#include "core/asset_paths.h"
#include "extract/material_sources.h"
#include "materials/donor_index.h"
#include "materials/fxmaterials.h"
#include "materials/techsets.h"
#include "materials_stage.h"
#include "modutil.h"
#include "shaders/runtime.h"
#include "textures/iwd_recovery.h"
#include "textures/iwi.h"
#include "weapons/weapon.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::IsFile;
    using mod::StartsWith;
    using mod::Str;
    using py::Json;

    namespace
    {
        const std::string ABSENT = "absent: ";
        // Texture slot -> BO2 code image with that slot's neutral value
        const std::map<std::string, std::string> ABSENT_IMAGE_SLOTS{
            {"normalMap", ",$identitynormalmap"}, {"specularMap", ",$black"}, {"colorMap", ",$white"}};

        struct Report
        {
            Json materials = Json::array(), images = Json::array(), errors = Json::array(), warnings = Json::array();
        };

        // iwi.read_iwi_header
        Json IwiHeaderJson(const std::string& blob)
        {
            if (blob.size() < 64 || blob.compare(0, 3, "IWi") != 0 || static_cast<unsigned char>(blob[3]) != 27)
                throw textures::TextureError("not an IWI v27 file");
            const auto i8 = [&](const size_t at) { return static_cast<int>(static_cast<signed char>(blob[at])); };
            const auto u16 = [&](const size_t at) { uint16_t v; std::memcpy(&v, blob.data() + at, 2); return v; };
            Json sizes = Json::array();
            for (int k = 0; k < 8; k++)
            {
                uint32_t v;
                std::memcpy(&v, blob.data() + 32 + 4 * k, 4);
                sizes.push_back(v);
            }
            return Json{{"format", i8(4)}, {"flags", i8(5)}, {"width", u16(6)}, {"height", u16(8)}, {"depth", u16(10)}, {"fileSizeForPicmip", sizes}};
        }

        std::set<std::string> TechsetShaders(const Json& techset)
        {
            std::set<std::string> files;
            for (const auto& tech : techset.value("techniques", Json::array()))
            {
                if (tech.is_null() || (tech.is_object() && tech.empty()))
                    continue;
                for (const auto& p : tech.value("passArray", Json::array()))
                {
                    const auto ps = p.contains("pixelShader") && p["pixelShader"].is_object() ? p["pixelShader"] : Json::object();
                    const auto vs = p.contains("vertexShader") && p["vertexShader"].is_object() ? p["vertexShader"] : Json::object();
                    if (ps.contains("name") && ps["name"].is_string() && !ps["name"].get<std::string>().empty())
                        files.insert("shader_bin/ps_" + ps["name"].get<std::string>() + ".cso");
                    if (vs.contains("name") && vs["name"].is_string() && !vs["name"].get<std::string>().empty())
                        files.insert("shader_bin/vs_" + vs["name"].get<std::string>() + ".cso");
                }
            }
            return files;
        }

        std::string Generic(const fs::path& p)
        {
            return mod::Generic(p);
        }
    } // namespace

    WeaponVisuals StageWeaponVisuals(const WeaponVisualsRequest& request, textures::IwdRecovery& recovery)
    {
        const auto& roots = request.roots;
        const auto& project = request.project;
        const auto& stockMaterials = request.stockMaterials;
        const auto& techsetDump = request.techsetDump;
        const auto bo2Raw = request.bo2Raw.value_or(project);
        const auto stageReport = mod::Json(project / "weapons.stage.json");
        std::set<std::string> names;
        for (const auto& n : stageReport["models"]["materials"])
            names.insert(n.get<std::string>());
        if (stageReport["dependencies"].contains("material"))
            for (const auto& n : stageReport["dependencies"]["material"])
                names.insert(n.get<std::string>());
        Report report;
        Json recovered = Json::object();
        std::map<std::string, materials::WawMaterial> effects;
        {
            const extract::MaterialSources sources(roots);
            for (const auto& name : names)
                if (auto material = sources.Find(name); material && materials::IsEffectTechset(material->techniqueSet))
                    effects.emplace(name, std::move(*material));
        }
        // t6bridge.stage_materials for the world/model technique sets
        std::set<std::string> used;
        {
            Json donors = Json::object();
            const auto indexed = materials::IndexDonors(stockMaterials);
            std::error_code e;
            for (const auto& [ts, path] : indexed.items())
                if (fs::exists(techsetDump / "techniquesets" / files::FromUtf8(ts + ".json"), e))
                    donors[ts] = path;
            if (donors.empty())
                throw StepError("StageError", "no stock T6 material donors found under " + Str(stockMaterials));
            MaterialStageRequest materialRequest{{}, roots, stockMaterials, project, techsetDump, "", donors, std::nullopt, true,
                                                 request.layeredVertexData, request.nativeFallbacks, {}};
            for (const auto& n : names)
            {
                if (!effects.count(n))
                    materialRequest.names.insert(n);
                materialRequest.rename.emplace(n, weapons::OutputName("material", n));
            }

            for (auto& result : StageMaterials(materialRequest))
            {
                for (const auto& i : result.images)
                    report.images.push_back(i);
                for (const auto& w : result.warnings)
                    report.warnings.push_back(w);
                if (!result.native)
                    for (const auto& x : result.errors)
                        report.errors.push_back(x);
                if (result.native || !result.entry.is_null())
                {
                    report.materials.push_back(std::move(result.entry));
                    used.insert(result.techset);
                }
            }
        }
        // effect materials on stock BO2 effect donors
        if (!effects.empty())
        {
            const auto donors = materials::IndexEffectDonors(stockMaterials, &techsetDump, bo2Raw);
            for (const auto& [name, source] : effects)
            {
                const materials::EffectDonor* donor = nullptr;
                std::vector<std::string> notes;
                Json material;
                try
                {
                    std::tie(donor, notes) = materials::ChooseEffectDonor(source, donors);
                    material = materials::BuildEffectMaterial(source, *donor, notes);
                }
                catch (const materials::TechniqueError& exc)
                {
                    report.errors.push_back("material " + name + ": " + exc.what());
                    continue;
                }
                shaders::FolderOutput output(project);
                const auto runtime = shaders::BindMaterial(source, material, roots, output, techsetDump);
                const auto outName = weapons::OutputName("material", name);
                const auto relative = assets::MaterialPath(outName);
                files::WriteText(project / relative, py::JsonDumps(material, 2));
                used.insert(material["techniqueSet"].get<std::string>());
                report.materials.push_back(Json{{"name", outName},
                                                {"source", name},
                                                {"file", Generic(relative)},
                                                {"t4_techset", source.techniqueSet},
                                                {"t6_techset", material["techniqueSet"]},
                                                {"shader_runtime", runtime},
                                                {"donor", Str(donor->path.lexically_relative(stockMaterials))},
                                                {"notes", notes}});
            }
        }
        std::set<std::string> translated;
        for (const auto& entry : report.materials)
            translated.insert(entry.contains("source") ? entry["source"].get<std::string>() : entry["name"].get<std::string>());
        Json failed = Json::object();
        for (const auto& n : names)
        {
            if (translated.count(n))
                continue;
            Json reasons = Json::array();
            for (const auto& err : report.errors)
            {
                const auto text = err.get<std::string>();
                if (StartsWith(text, "material " + n + ":") || StartsWith(text, "material " + n + " ("))
                    reasons.push_back(text);
            }
            failed[n] = reasons.empty() ? Json::array({"not translated"}) : reasons;
        }
        // _stage_image: the original pixels, through the IWD recovery
        std::map<std::string, std::optional<std::string>> imageStatus;
        const auto stageImage = [&](const std::string& name, const std::string& output) -> std::optional<std::string>
        {
            const auto relative = assets::ImagePath(name, ".dds");
            const auto stem = Generic(relative.lexically_relative("images").replace_extension());
            std::optional<fs::path> source;
            for (const auto& root : roots)
                if (IsFile(root / relative))
                {
                    source = root / relative;
                    break;
                }
            textures::RecoveredImage resolved;
            try
            {
                resolved = recovery.Resolve(stem, source ? *source : fs::path());
            }
            catch (const std::exception& exc)
            {
                const auto error = "weapon image " + name + ": " + exc.what();
                report.errors.push_back(error);
                return error;
            }
            if (!resolved.archive.empty())
            {
                Json metadata{{"archive", files::Utf8(resolved.archive)}, {"entry", resolved.entry}, {"iwi_format", resolved.iwiFormat}};
                if (resolved.decoderTables)
                    metadata["decoder_tables"] = files::Utf8(*resolved.decoderTables);
                recovered[stem] = metadata;
            }
            if (resolved.path.empty())
            {
                const auto error = "weapon image " + name + ": original pixels absent from supplied zone dumps and IWDs";
                report.errors.push_back(error);
                return ABSENT + error;
            }
            Json header;
            try
            {
                const auto blob = textures::DdsToIwi(mod::Bytes(resolved.path)).bytes;
                files::WriteBytes(project / assets::ImagePath(output, ".iwi"), blob);
                header = IwiHeaderJson(blob);
            }
            catch (const std::exception& exc)
            {
                const auto error = "weapon image " + name + ": " + exc.what();
                report.errors.push_back(error);
                return error;
            }
            Json image{{"name", output}, {"source", Str(resolved.path)}};
            for (const auto& [k, v] : header.items())
                image[k] = v;
            if (recovered.contains(stem))
                image["iwd_recovery"] = recovered[stem];
            report.images.push_back(image);
            return std::nullopt;
        };
        std::set<std::string> synthesized;
        Json materialImages = Json::object();
        for (const auto& entry : report.materials)
        {
            const auto wawMaterial = entry.contains("source") ? entry["source"].get<std::string>() : entry["name"].get<std::string>();
            const auto path = project / files::FromUtf8(entry["file"].get<std::string>());
            auto material = mod::Json(path);
            Json images = Json::array();
            if (material.contains("textures"))
                for (auto& texture : material["textures"])
                {
                    const auto name = texture.value("image", std::string());
                    if (name.empty())
                        continue;
                    if (StartsWith(name, ",") || StartsWith(name, "$"))
                    {
                        texture["image"] = StartsWith(name, ",") ? name.substr(1) : name;
                        images.push_back("," + texture["image"].get<std::string>());
                        continue;
                    }
                    if (mod::Truthy(entry, "native_equivalent"))
                    {
                        // stage_materials already copied the native IWI and namespaced its binding
                        images.push_back(name);
                        continue;
                    }
                    const auto output = weapons::OutputName("image", name);
                    if (!imageStatus.count(output))
                        imageStatus[output] = stageImage(name, output);
                    const auto status = imageStatus[output];
                    const auto slot = texture.value("name", std::string());
                    const auto code = ABSENT_IMAGE_SLOTS.find(slot);
                    if (status && StartsWith(*status, ABSENT) && code != ABSENT_IMAGE_SLOTS.end())
                    {
                        // the pixels exist nowhere in WaW: a BO2 code image of the slot's neutral value
                        texture["image"] = code->second.substr(1);
                        images.push_back(code->second);
                        report.warnings.push_back("BO2_FALLBACK image " + name + " -> " + code->second + ": " + slot + " of material " + wawMaterial + "; " +
                                                  status->substr(ABSENT.size()));
                        continue;
                    }
                    if (status && StartsWith(*status, ABSENT))
                    {
                        // a slot with no neutral code image: WaW drew its default image there
                        if (!synthesized.count(output))
                        {
                            const auto why = std::string("no pixel data in any zone or IWD (WaW draws its default image); neutral grey");
                            std::string pixels;
                            for (int k = 0; k < 16; k++)
                                pixels += std::string("\x80\x80\x80\xff", 4);
                            files::WriteBytes(project / assets::ImagePath(output, ".iwi"), textures::RgbaToIwi(4, 4, pixels).bytes);
                            report.warnings.push_back("image " + output + ": synthesized, " + why);
                            report.images.push_back(Json{{"name", output}, {"source", nullptr}, {"synthesized", why}});
                            synthesized.insert(output);
                            Json kept = Json::array();
                            for (const auto& err : report.errors)
                                if (err.get<std::string>() != status->substr(ABSENT.size()))
                                    kept.push_back(err);
                            report.errors = kept;
                        }
                        texture["image"] = output;
                        images.push_back(output);
                        continue;
                    }
                    texture["image"] = output;
                    images.push_back(output);
                    if (status)
                    {
                        if (!failed.contains(wawMaterial))
                            failed[wawMaterial] = Json::array();
                        failed[wawMaterial].push_back(StartsWith(*status, ABSENT) ? status->substr(ABSENT.size()) : *status);
                    }
                }
            materialImages[entry["name"].get<std::string>()] = images;
            files::WriteText(path, py::JsonDumps(material, 2) + "\n");
        }
        // the technique sets and shader programs the materials use
        for (const auto& name : used)
        {
            const fs::path relative = fs::path("techniquesets") / files::FromUtf8(name + ".json");
            const auto source = IsFile(project / relative) ? project / relative : techsetDump / relative;
            if (!IsFile(source))
            {
                report.errors.push_back("weapon technique set " + name + ": missing from " + Str(techsetDump));
                continue;
            }
            std::set<std::string> destinations{Generic(relative)};
            for (const auto& shader : TechsetShaders(mod::Json(source)))
                destinations.insert(shader);
            for (const auto& rel : destinations)
            {
                const auto dst = project / files::FromUtf8(rel);
                if (IsFile(dst))
                    continue;
                const auto from = techsetDump / files::FromUtf8(rel);
                if (!IsFile(from))
                {
                    report.errors.push_back("weapon shader " + Str(fs::path(files::FromUtf8(rel))) + ": missing from " + Str(techsetDump));
                    continue;
                }
                fs::create_directories(dst.parent_path());
                fs::copy_file(from, dst, fs::copy_options::overwrite_existing);
                fs::last_write_time(dst, fs::last_write_time(from));
            }
        }
        Json result{{"status", "staged_not_native_validated"},
                    {"failed_materials", failed},
                    {"material_images", materialImages},
                    {"project", "weapon_visuals"},
                    {"materials", report.materials},
                    {"images", report.images},
                    {"techsets", Json::array()},
                    {"collision", Json::object()},
                    {"entities", Json::object()},
                    {"scripts", Json::object()},
                    {"content", Json::object()},
                    {"fx_table", Json::object()},
                    {"errors", report.errors},
                    {"warnings", report.warnings}};
        files::WriteText(project / "weapons.visuals.json", py::JsonDumps(result, 2) + "\n");
        return {report.warnings.get<std::vector<std::string>>(), report.errors.size(), std::move(recovered)};
    }
} // namespace w2b::pipeline
