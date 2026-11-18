// t6bridge.stage_effects (see effects_stage.h).
#include "effects_stage.h"

#include "core/asset_paths.h"
#include "core/zip.h"
#include "effects/stage.h"
#include "extract/stockwaw.h"
#include "materials/donor_index.h"
#include "materials/fxmaterials.h"
#include "materials/techsets.h"
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
    using py::Json;

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

    Json StageEffectsStep(const Json& request, textures::IwdRecovery& recovery)
    {
        if (request.value("source_waw", false))
            throw StepError("NotImplementedError", "effects compiled from WaW Mod Tools sources are staged by the Python step");
        const auto names = request["names"];
        auto roots = Paths(request["roots"]);
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        const auto stockMaterials = files::FromUtf8(request["stock_materials"].get<std::string>());
        const std::optional<fs::path> techsetDump = request["techset_dump"].is_null() ? std::nullopt : std::optional(files::FromUtf8(request["techset_dump"].get<std::string>()));
        const auto bo2Root = files::FromUtf8(request["bo2_root"].get<std::string>());
        IwdFormats formats(Paths(request["iwd_dirs"]));
        auto stock = extract::StockWaw::FromJson(request.value("stock", Json()));
        Json warnings = Json::array(), errors = Json::array(), materialsReport = Json::array(), images = Json::array(), recovered = Json::object();
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
        // fx.closure: every script effect plus everything those effects reference
        std::map<std::string, fs::path> effects;
        std::map<std::string, std::string> absent;
        Deps deps;
        {
            std::set<std::string> start;
            for (const auto& n : names)
                start.insert(n.get<std::string>());
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
        for (const auto& [name, parent] : absent)
            warnings.push_back("WAW_FX_ABSENT " + name + " (loaded by " + parent + "): no zone of the WaW install defines it, so WaW plays nothing either; skipped");
        // the materials the effects draw with
        const auto allRoots = [&]
        {
            auto all = roots;
            all.insert(all.end(), extraRoots.begin(), extraRoots.end());
            return all;
        };
        auto all = allRoots();
        Json materialNames = Json::object(), worldMaterials = Json::object();
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
            const auto t4 = ReadJson(*src);
            const auto ts = t4.value("techniqueSet", std::string());
            std::vector<std::string> undecodable;
            for (const auto& tex : t4.value("textures", Json::array()))
            {
                const auto img = tex.value("image", std::string());
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
            Json out;
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
            std::vector<std::tuple<std::string, std::string, Json, bool>> variants{{outName, mat, out, false}};
            if (deps.trailMaterials.count(mat) && techsetDump)
                variants.emplace_back(outName + FX_TRAIL_MATERIAL_SUFFIX, mat + "#trail", out, true);
            for (auto& [name, key, material, trail] : variants)
            {
                const auto runtime = techsetDump ? shaders::BindMaterial(t4, material, all, projectRoot, *techsetDump, std::nullopt, true, trail)
                                                 : Json{{"active", Json::array()}, {"unsupported", Json::array({"native technique dump absent"})}};
                if (trail && !mod::Truthy(runtime, "face_on_falloff"))
                    continue; // no view falloff: trails share the ordinary material
                if (material.contains("textures"))
                    for (auto& tex : material["textures"])
                    {
                        const auto image = tex.value("image", std::string());
                        if (!image.empty() && !StartsWith(image, ",") && !StartsWith(image, "$"))
                            tex["image"] = FX_IMAGE_PREFIX + image;
                    }
                const auto outRel = assets::MaterialPath(name);
                const auto dst = projectRoot / outRel;
                fs::create_directories(dst.parent_path());
                files::WriteText(dst, py::JsonDumps(material, 2) + "\n");
                techsets.insert(material["techniqueSet"].get<std::string>());
                for (const auto& t : material.value("textures", Json::array()))
                    if (t.contains("image") && t["image"].is_string() && !t["image"].get<std::string>().empty())
                        effectImages.insert(t["image"].get<std::string>());
                materialNames[key] = name;
                Json entryNotes = Json(notes);
                if (trail)
                    entryNotes.push_back("trail geometry: WaW view falloff evaluated face-on");
                materialsReport.push_back(Json{{"name", name},
                                               {"source", mat},
                                               {"file", Generic(outRel)},
                                               {"t4_techset", ts},
                                               {"t6_techset", material["techniqueSet"]},
                                               {"shader_runtime", runtime},
                                               {"donor", Generic(donor->path.lexically_relative(stockMaterials))},
                                               {"notes", entryNotes}});
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
            Json names = Json::array();
            for (const auto& [k, _] : worldMaterials.items())
                names.push_back(k);
            Json rootsJson = Json::array();
            for (const auto& r : all)
                rootsJson.push_back(Str(r));
            const auto scratch = fs::temp_directory_path() / ("w2b_effect_materials_" + std::to_string(GetCurrentProcessId()));
            const Json materialRequest{{"names", names},
                                       {"roots", rootsJson},
                                       {"stock_materials", Str(stockMaterials)},
                                       {"project_root", Str(projectRoot)},
                                       {"techset_root", techsetDump ? Json(Str(*techsetDump)) : Json()},
                                       {"rename", worldMaterials},
                                       {"image_prefix", "waw_world/"},
                                       {"donors", donorsJson},
                                       {"falloff_placement", nullptr},
                                       {"light_shadows", true},
                                       {"layered_vertex_data", request.value("layered_vertex_data", false)},
                                       {"native_fallbacks", Json::object()},
                                       {"scratch", Str(scratch / "work")}};
            fs::create_directories(scratch);
            files::WriteText(scratch / "request.json", py::JsonDumpsDefault(materialRequest));
            const auto staged = StageMaterials(scratch / "request.json");
            fs::remove_all(scratch, e);
            for (const auto& result : staged["results"])
            {
                for (const auto& i : result["images"])
                    images.push_back(i);
                for (const auto& w : result["warnings"])
                    warnings.push_back(w);
                for (const auto& x : result["errors"])
                    errors.push_back(x);
                if (!result["entry"].is_null())
                {
                    materialsReport.push_back(result["entry"]);
                    techsets.insert(result["techset"].get<std::string>());
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
            files::WriteText(path, py::JsonDumps(Json{{"streamingMode", modes}}, 2) + "\n");
        }
        // fx.stage_converted
        Json converted = Json{{"converted", Json::array()}, {"failed", 0}, {"warnings", Json::array()}, {"models", Json::array()}, {"sounds", Json::array()}};
        if (!effects.empty())
        {
            Json list = Json::array();
            for (const auto& [name, source] : effects)
                list.push_back(Json{{"name", name}, {"source", Str(source)}});
            Json absentList = Json::array();
            for (const auto& [name, _] : absent)
                absentList.push_back(name);
            const auto scratch = fs::temp_directory_path() / ("w2b_effects_" + std::to_string(GetCurrentProcessId()));
            fs::create_directories(scratch);
            files::WriteText(scratch / "convert.json", py::JsonDumpsDefault(Json{{"output", Str(projectRoot)},
                                                                                  {"effects", list},
                                                                                  {"absent", absentList},
                                                                                  {"materialNames", materialNames},
                                                                                  {"failedMaterials", std::vector<std::string>(failedMaterials.begin(), failedMaterials.end())},
                                                                                  {"workers", request.value("workers", 1)}}));
            converted = effects::StageEffects(scratch / "convert.json");
            fs::remove_all(scratch, e);
        }
        for (const auto& w : converted["warnings"])
            warnings.push_back(w);
        std::set<std::string> done;
        for (const auto& c : converted["converted"])
            done.insert(c.get<std::string>());
        Json table = Json::object(), zoneFx = Json::array();
        std::set<std::string> scriptNames;
        for (const auto& n : names)
            scriptNames.insert(n.get<std::string>());
        for (const auto& name : scriptNames)
            if (done.count(name))
            {
                table[name] = "waw/" + name;
                zoneFx.push_back("waw/" + name);
            }
        for (const auto& [key, zone] : stockUsed)
            warnings.push_back("WAW_STOCK_ASSET " + key.first + " " + key.second + ": the map only references it; ported from the stock WaW zone " + zone + ".ff");
        warnings.push_back("fx: " + std::to_string(done.size()) + " WaW effects converted (" + std::to_string(table.size()) + " loaded by scripts), " +
                           std::to_string(converted["failed"].get<long long>()) + " failed, " + std::to_string(absent.size()) + " absent from WaW");
        Json extra = Json::array();
        for (const auto& r : extraRoots)
            extra.push_back(Str(r));
        return Json{{"warnings", warnings},
                    {"errors", errors},
                    {"materials", materialsReport},
                    {"images", images},
                    {"table", table},
                    {"zone_fx", zoneFx},
                    {"models", converted["models"]},
                    {"sounds", converted["sounds"]},
                    {"extra_roots", extra},
                    {"techsets", std::vector<std::string>(techsets.begin(), techsets.end())},
                    {"recovered", recovered},
                    {"stock_prepared", stock ? stock->PreparedJson() : Json()}};
    }
} // namespace w2b::pipeline
