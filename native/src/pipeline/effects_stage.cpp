// t6bridge.stage_effects (see effects_stage.h).
#include "effects_stage.h"

#include "core/asset_paths.h"
#include "core/zip.h"
#include "effects/stage.h"
#include "extract/stockwaw.h"
#include "extract/wawsource.h"
#include "materials/donor_index.h"
#include "materials/fxmaterials.h"
#include "materials/techsets.h"
#include "materials/waw_material_json.h"
#include "materials_stage.h"
#include "modutil.h"
#include "shaders/runtime.h"
#include "textures/iwd_recovery.h"
#include "textures/dds.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::Bytes;
    using mod::Generic;
    using mod::IsDir;
    using mod::IsFile;
    using mod::Lower;
    using mod::StartsWith;
    using mod::Str;
    using util::Json;

    namespace
    {
        const std::string FX_MATERIAL_PREFIX = "waw_fx/", FX_TRAIL_MATERIAL_SUFFIX = "__w2b_trail", FX_IMAGE_PREFIX = "waw_fx/";
        constexpr int T4_TRAIL = 4, EFFECT_IMAGE_STREAMING = 2;
        const std::map<int, std::string> IWI6_WAVELET_FORMATS{{6, "wavelet RGBA"}, {7, "wavelet RGB"}, {8, "wavelet luminance alpha"}, {9, "wavelet luminance"}, {10, "wavelet alpha"}};

        Json ReadJson(const fs::path& p)
        {
            return mod::Json(p);
        }

        std::vector<fs::path> Paths(const Json& values)
        {
            std::vector<fs::path> out;
            for (const auto& v : values)
                out.push_back(files::FromUtf8(v.get<std::string>()));
            return out;
        }

        std::optional<fs::path> FindAny(const std::vector<fs::path>& roots, const fs::path& relative)
        {
            std::error_code e;
            for (const auto& r : roots)
                if (fs::exists(r / relative, e))
                    return r / relative;
            return std::nullopt;
        }

        fs::path FxFile(const fs::path& root, const std::string& name)
        {
            return root / "fx" / files::FromUtf8(name + ".w2bfx.json");
        }

        struct Deps
        {
            std::set<std::string> effects, materials, trailMaterials, models, sounds;
        };

        // fx.dependencies
        Deps Dependencies(const Json& t4)
        {
            Deps d;
            const auto add = [](std::set<std::string>& set, const Json& e, const char* key)
            {
                if (e.contains(key) && e[key].is_string() && !e[key].get<std::string>().empty())
                    set.insert(e[key].get<std::string>());
            };
            for (const auto& e : t4["elemDefs"])
            {
                for (const char* k : {"effectOnImpact", "effectOnDeath", "effectEmitted"})
                    add(d.effects, e, k);
                for (const auto& v : e["visuals"])
                {
                    add(d.effects, v, "effect");
                    if (v.contains("material") && v["material"].is_string() && !v["material"].get<std::string>().empty())
                    {
                        d.materials.insert(v["material"].get<std::string>());
                        if (e["elemType"] == T4_TRAIL)
                            d.trailMaterials.insert(v["material"].get<std::string>());
                    }
                    for (const auto& m : v.value("materials", Json::array()))
                        if (m.is_string() && !m.get<std::string>().empty())
                            d.materials.insert(m.get<std::string>());
                    add(d.models, v, "model");
                    add(d.sounds, v, "sound");
                }
            }
            return d;
        }

        // iwd_image_format: IWI v6 pixel format of images/<stem>.iwi in the first IWD holding it
        class IwdFormats
        {
        public:
            explicit IwdFormats(std::vector<fs::path> dirs) : dirs_(std::move(dirs))
            {
            }

            std::optional<int> Format(const std::string& stem)
            {
                const auto wanted = "images/" + Lower(stem) + ".iwi";
                for (const auto& d : dirs_)
                {
                    std::vector<fs::path> archives;
                    std::error_code e;
                    if (IsDir(d))
                        for (const auto& entry : fs::directory_iterator(d, e))
                            if (Lower(files::Utf8(entry.path().extension())) == ".iwd")
                                archives.push_back(entry.path());
                    std::sort(archives.begin(), archives.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
                    for (const auto& iwd : archives)
                    {
                        auto& archive = Open(iwd);
                        for (const auto& entry : archive.Entries())
                            if (Lower(entry.name) == wanted)
                            {
                                const auto head = archive.Read(entry).substr(0, 5);
                                if (head.size() == 5 && head.substr(0, 3) == "IWi" && head[3] == 6)
                                    return static_cast<unsigned char>(head[4]);
                            }
                    }
                }
                return std::nullopt;
            }

        private:
            zip::Archive& Open(const fs::path& path)
            {
                const auto key = Str(path);
                auto it = open_.find(key);
                if (it == open_.end())
                    it = open_.emplace(key, std::make_unique<zip::Archive>(path)).first;
                return *it->second;
            }
            std::vector<fs::path> dirs_;
            std::map<std::string, std::unique_ptr<zip::Archive>> open_;
        };

        Json RecoveredJson(const textures::RecoveredImage& image)
        {
            Json metadata{{"archive", files::Utf8(image.archive)}, {"entry", image.entry}, {"iwi_format", image.iwiFormat}};
            if (image.decoderTables)
                metadata["decoder_tables"] = files::Utf8(*image.decoderTables);
            return metadata;
        }
    } // namespace

    EffectsStage StageEffectsStep(const EffectsStageRequest& request, textures::IwdRecovery& recovery)
    {
        const auto& names = request.names;
        auto roots = request.roots;
        const auto& projectRoot = request.projectRoot;
        shaders::FolderOutput projectOutput(projectRoot);
        const auto& stockMaterials = request.stockMaterials;
        const auto& techsetDump = request.techsetDump;
        const auto& bo2Root = request.bo2Root;
        IwdFormats formats(request.iwdDirs);
        auto* stock = request.stock;
        auto* modToolsSource = request.source;
        Json warnings = Json::array(), errors = Json::array(), images = Json::array(), recovered = Json::object();
        std::vector<MaterialRecord> materialsReport;
        std::vector<fs::path> extraRoots;
        std::set<std::string> techsets;
        std::map<std::pair<std::string, std::string>, std::string> stockUsed;
        std::error_code e;
        // converted output of earlier runs
        for (const auto& old : {projectRoot / "fx", projectRoot / "materials" / "waw_fx"})
            fs::remove_all(old, e);
        const auto fromStock = [&](const std::string& kind, const fs::path& rel) -> std::optional<fs::path>
        {
            if (!stock)
                return std::nullopt;
            auto name = Generic(rel);
            const auto strip = [&](const std::string& prefix, const std::string& suffix)
            {
                if (StartsWith(name, prefix))
                    name.erase(0, prefix.size());
                if (mod::EndsWith(name, suffix))
                    name.resize(name.size() - suffix.size());
            };
            if (kind == "fx")
                strip("fx/", ".w2bfx.json");
            else if (kind == "material")
                strip("materials/", ".json");
            else if (kind == "xmodel")
                strip("xmodel/", ".json");
            const auto hit = stock->RootFor(kind, name);
            if (!hit || !fs::exists(hit->second / rel, e))
                return std::nullopt;
            stockUsed[{kind, name}] = hit->first;
            if (std::find(extraRoots.begin(), extraRoots.end(), hit->second) == extraRoots.end())
                extraRoots.push_back(hit->second);
            return hit->second / rel;
        };
        // GUIDELINES 27 source 4: only asked for effects no WaW zone defines;
        // effect compiled from Mod Tools source -> its linker warnings
        std::map<std::string, std::vector<std::string>> sourceUsed;
        const auto fromSource = [&](const std::string& name) -> std::optional<fs::path>
        {
            const auto rel = fs::path("fx") / files::FromUtf8(name + ".w2bfx.json");
            std::optional<fs::path> root;
            try
            {
                root = modToolsSource->Compile("fx", name);
            }
            catch (const extract::WawSourceError& error)
            {
                errors.push_back("fx " + name + ": " + error.what());
                return std::nullopt;
            }
            if (!root || !fs::exists(*root / rel, e))
                return std::nullopt;
            sourceUsed[name] = modToolsSource->LinkWarnings("fx", name);
            if (std::find(extraRoots.begin(), extraRoots.end(), *root) == extraRoots.end())
                extraRoots.push_back(*root);
            return *root / rel;
        };
        // fx.closure: every script effect plus everything those effects reference
        std::map<std::string, fs::path> effects;
        std::map<std::string, std::string> absent;
        Deps deps;
        {
            std::set<std::string> start;
            start.insert(names.begin(), names.end());
            std::vector<std::pair<std::string, std::string>> stack;
            for (const auto& n : start)
                stack.emplace_back(n, "script");
            while (!stack.empty())
            {
                const auto [name, parent] = stack.back();
                stack.pop_back();
                if (effects.count(name) || absent.count(name))
                    continue;
                std::optional<fs::path> src;
                for (const auto& r : roots)
                    if (fs::exists(FxFile(r, name), e))
                    {
                        src = FxFile(r, name);
                        break;
                    }
                if (!src)
                    src = fromStock("fx", fs::path("fx") / files::FromUtf8(name + ".w2bfx.json"));
                if (!src && modToolsSource)
                    src = fromSource(name);
                if (!src)
                {
                    absent[name] = parent;
                    continue;
                }
                effects[name] = *src;
                const auto d = Dependencies(ReadJson(*src));
                deps.materials.insert(d.materials.begin(), d.materials.end());
                deps.trailMaterials.insert(d.trailMaterials.begin(), d.trailMaterials.end());
                deps.models.insert(d.models.begin(), d.models.end());
                deps.sounds.insert(d.sounds.begin(), d.sounds.end());
                for (const auto& x : d.effects)
                    stack.emplace_back(x, name);
            }
        }
        const std::string where = modToolsSource ? "no zone of the WaW install and no WaW Mod Tools source" : "no zone of the WaW install";
        for (const auto& [name, parent] : absent)
            warnings.push_back("WAW_FX_ABSENT " + name + " (loaded by " + parent + "): " + where + " defines it, so WaW plays nothing either; skipped");
        for (const auto& [name, missing] : sourceUsed)
        {
            warnings.push_back("WAW_SOURCE_ASSET fx " + name + ": in no compiled WaW zone; compiled from WaW Mod Tools raw/fx with the WaW linker");
            for (const auto& line : missing)
                warnings.push_back("fx " + name + ": WaW linker: " + line);
        }
        // the materials the effects draw with
        const auto allRoots = [&]
        {
            auto all = roots;
            all.insert(all.end(), extraRoots.begin(), extraRoots.end());
            return all;
        };
        auto all = allRoots();
        effects::MaterialNames materialNames;
        Json worldMaterials = Json::object();
        const auto donors = materials::IndexEffectDonors(stockMaterials, techsetDump ? &*techsetDump : nullptr, bo2Root / "raw");
        std::set<std::string> failedMaterials, effectImages;
        for (const auto& mat : deps.materials)
        {
            const auto rel = assets::MaterialPath(mat);
            auto src = FindAny(all, rel);
            if (!src)
                src = fromStock("material", rel);
            all = allRoots();
            const auto outName = FX_MATERIAL_PREFIX + mat;
            if (!src)
            {
                warnings.push_back("WAW_ASSET_ABSENT fx material " + mat + ": in no WaW zone (map, companions or stock install); effects using it are not converted");
                failedMaterials.insert(mat);
                continue;
            }
            const auto t4 = materials::FromOatJson(ReadJson(*src));
            const auto& ts = t4.techniqueSet;
            std::vector<std::string> undecodable;
            for (const auto& tex : t4.textures)
            {
                const auto& img = tex.image;
                if (img.empty() || StartsWith(img, "$"))
                    continue;
                auto stem = img;
                std::replace(stem.begin(), stem.end(), '*', '_');
                if (std::any_of(all.begin(), all.end(), [&](const fs::path& r) { return fs::exists(r / "images" / files::FromUtf8(stem + ".dds"), e); }))
                    continue;
                const auto fmt = formats.Format(stem);
                if (fmt && IWI6_WAVELET_FORMATS.count(*fmt))
                {
                    try
                    {
                        const auto image = recovery.Recover(stem);
                        if (!image.archive.empty())
                            recovered[stem] = RecoveredJson(image);
                        if (image.path.empty())
                            undecodable.push_back(img + " (" + IWI6_WAVELET_FORMATS.at(*fmt) + " IWI): decoder unavailable");
                    }
                    catch (const std::exception& exc)
                    {
                        undecodable.push_back(img + ": " + exc.what());
                    }
                }
            }
            if (!undecodable.empty())
            {
                warnings.push_back("UNSUPPORTED_IMAGE fx material " + mat + ": " + mod::Join(undecodable, ", "));
                failedMaterials.insert(mat);
                continue;
            }
            if (!materials::IsEffectTechset(ts))
            {
                worldMaterials[mat] = outName; // decals etc.: world/model technique sets
                materialNames[mat] = outName;
                continue;
            }
            const materials::EffectDonor* donor = nullptr;
            std::vector<std::string> notes;
            materials::T6Material out;
            try
            {
                std::tie(donor, notes) = materials::ChooseEffectDonor(t4, donors);
                out = materials::BuildEffectMaterial(t4, *donor, notes);
            }
            catch (const materials::TechniqueError& exc)
            {
                errors.push_back("fx material " + mat + " (" + ts + "): " + exc.what());
                failedMaterials.insert(mat);
                continue;
            }
            std::vector<std::tuple<std::string, std::string, materials::T6Material, bool>> variants{{outName, mat, out, false}};
            if (deps.trailMaterials.count(mat) && techsetDump)
                variants.emplace_back(outName + FX_TRAIL_MATERIAL_SUFFIX, mat + "#trail", out, true);
            for (auto& [name, key, material, trail] : variants)
            {
                const auto runtime = techsetDump ? shaders::BindMaterial(t4, material, all, projectOutput, *techsetDump, std::nullopt, true, trail)
                                                 : Json{{"active", Json::array()}, {"unsupported", Json::array({"native technique dump absent"})}};
                if (trail && !mod::Truthy(runtime, "face_on_falloff"))
                    continue; // no view falloff: trails share the ordinary material
                for (auto& tex : material.textures)
                {
                    const auto& image = tex.image;
                    if (!image.empty() && !StartsWith(image, ",") && !StartsWith(image, "$"))
                        tex.image = FX_IMAGE_PREFIX + image;
                }
                const auto outRel = assets::MaterialPath(name);
                const auto dst = projectRoot / outRel;
                fs::create_directories(dst.parent_path());
                files::WriteText(dst, materials::T6MaterialText(material));
                techsets.insert(material.techniqueSet);
                for (const auto& t : material.textures)
                    if (!t.image.empty())
                        effectImages.insert(t.image);
                materialNames[key] = name;
                MaterialRecord record;
                record.name = name;
                record.source = mat;
                record.file = Generic(outRel);
                record.t4Techset = std::optional(ts);
                record.t6Techset = material.techniqueSet;
                record.shaderRuntime = runtime;
                record.donor = Generic(donor->path.lexically_relative(stockMaterials));
                record.notes = notes;
                if (trail)
                    record.notes->emplace_back("trail geometry: WaW view falloff evaluated face-on");
                materialsReport.push_back(std::move(record));
            }
        }
        if (!worldMaterials.empty())
        {
            // world/model technique sets (stage_materials with the effect names)
            Json donorsJson = Json::object();
            const auto indexed = materials::IndexDonors(stockMaterials);
            for (const auto& [ts, path] : indexed.items())
                if (!techsetDump || fs::exists(*techsetDump / "techniquesets" / files::FromUtf8(ts + ".json"), e))
                    donorsJson[ts] = path;
            if (donorsJson.empty())
                throw StepError("StageError", "no stock T6 material donors found under " + Str(stockMaterials));
            MaterialStageRequest materialRequest{{}, all, stockMaterials, projectRoot, techsetDump, "waw_world/", donorsJson, std::nullopt,
                                                 true, request.layeredVertexData, {}, {}};
            for (const auto& [name, staged] : worldMaterials.items())
            {
                materialRequest.names.insert(name);
                materialRequest.rename.emplace(name, staged.get<std::string>());
            }
            for (auto& result : StageMaterials(materialRequest))
            {
                for (const auto& i : result.images)
                    images.push_back(i);
                for (const auto& w : result.warnings)
                    warnings.push_back(w);
                for (const auto& x : result.errors)
                    errors.push_back(x);
                if (result.entry)
                {
                    materialsReport.push_back(std::move(*result.entry));
                    techsets.insert(result.techset);
                }
            }
        }
        // write_image_streaming: effect images use streaming mode 2
        {
            Json modes = Json::object();
            for (const auto& image : effectImages)
                modes[image] = EFFECT_IMAGE_STREAMING;
            const auto path = projectRoot / "images" / "streaming.json";
            fs::create_directories(path.parent_path());
            files::WriteText(path, util::JsonDumps(Json{{"streamingMode", modes}}, 2) + "\n");
        }
        // fx.stage_converted
        effects::EffectConversion converted;
        if (!effects.empty())
        {
            effects::EffectRequest convert{projectRoot, {effects.begin(), effects.end()}, {}, materialNames, failedMaterials, request.workers};
            for (const auto& [name, _] : absent)
                convert.absent.insert(name);
            converted = effects::StageEffects(convert);
        }
        for (const auto& w : converted.warnings)
            warnings.push_back(w);
        const auto& done = converted.converted;
        std::map<std::string, std::string> table;
        std::vector<std::string> zoneFx;
        const std::set<std::string> scriptNames(names.begin(), names.end());
        for (const auto& name : scriptNames)
            if (done.count(name))
            {
                table[name] = "waw/" + name;
                zoneFx.push_back("waw/" + name);
            }
        for (const auto& [key, zone] : stockUsed)
            warnings.push_back("WAW_STOCK_ASSET " + key.first + " " + key.second + ": the map only references it; ported from the stock WaW zone " + zone + ".ff");
        warnings.push_back("fx: " + std::to_string(done.size()) + " WaW effects converted (" + std::to_string(table.size()) + " loaded by scripts), " +
                           std::to_string(converted.failed) + " failed, " + std::to_string(absent.size()) + " absent from WaW");
        return {warnings.get<std::vector<std::string>>(), errors.get<std::vector<std::string>>(), std::move(materialsReport), std::move(images),
                std::move(table), std::move(zoneFx), std::move(converted.models), std::move(converted.sounds), std::move(extraRoots),
                std::move(techsets), std::move(recovered)};
    }
} // namespace w2b::pipeline
