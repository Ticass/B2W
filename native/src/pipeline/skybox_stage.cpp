// The map's own sky dome (see skybox_stage.h).
#include "skybox_stage.h"

#include "core/asset_paths.h"
#include "modutil.h"
#include "shaders/runtime.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <set>

namespace w2b::pipeline
{
    using mod::IsFile;
    using mod::StartsWith;
    using py::Json;

    namespace
    {
        const std::string SKYBOX_MATERIAL = "mc/mtl_dlc0_zm_skybox_nuketown";
        const std::vector<std::string> STATE_FIELDS{"srcBlendRgb",   "dstBlendRgb",     "blendOpRgb", "srcBlendAlpha", "dstBlendAlpha", "blendOpAlpha", "alphaTest",
                                                    "cullFace",      "colorWriteRgb",   "colorWriteAlpha", "depthTest",  "depthWrite",    "polygonOffset"};

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
        const Json* MainState(const Json& material)
        {
            if (material.contains("stateBits"))
                for (const auto& s : material["stateBits"])
                    if (!mod::Truthy(s, "polymodeLine") && !(s.contains("depthTest") && s["depthTest"] == "disabled"))
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
    } // namespace

    Json StageSourceSkybox(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto project = request["project"].get<std::string>();
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        std::vector<fs::path> roots;
        for (const auto& r : request["roots"])
            roots.push_back(files::FromUtf8(r.get<std::string>()));
        const auto sourceName = request["source_name"].get<std::string>();
        const auto nativeRoot = files::FromUtf8(request["native_root"].get<std::string>());
        const auto bo2 = files::FromUtf8(request["bo2_root"].get<std::string>());
        try
        {
            const fs::path modelRelative = fs::path("xmodel") / files::FromUtf8(sourceName + ".json");
            const auto sourceFile = Find(roots, modelRelative);
            if (!sourceFile)
                throw StageError("source sky model " + sourceName + " missing");
            auto model = mod::Json(*sourceFile);
            const auto donor = mod::Json(bo2 / "raw/materials" / files::FromUtf8(SKYBOX_MATERIAL + ".json"));
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
            Json materials = Json::array();
            for (const auto& name : materialNames)
            {
                const auto materialFile = Find(roots, assets::MaterialPath(name));
                if (!materialFile)
                    throw StageError("source sky material " + name + " missing");
                const auto source = mod::Json(*materialFile);
                auto target = donor;
                target["textures"] = source.at("textures");
                target["constants"] = source.value("constants", Json::array());
                target["sortKey"] = source.at("sortKey");
                if (const auto* state = MainState(source); state && !state->empty())
                    for (auto& native : target["stateBits"])
                        if (!mod::Truthy(native, "polymodeLine"))
                            for (const auto& key : STATE_FIELDS)
                                if (state->contains(key))
                                    native[key] = (*state)[key];
                const auto runtime = shaders::BindMaterial(source, target, roots, projectRoot, nativeRoot);
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
                files::WriteText(projectRoot / relative, py::JsonDumps(target, 2) + "\n");
                materials.push_back(Json{{"name", name}, {"file", mod::Generic(relative)}, {"t6_techset", target["techniqueSet"]}, {"shader_runtime", runtime}});
            }
            model["_game"] = "t6";
            BridgeBoneBounds(model);
            model["flags"] = 0x200000;
            model["lightingOriginOffset"] = Json{{"x", 0.0}, {"y", 0.0}, {"z", 0.5}};
            model["lightingOriginRange"] = 0.5;
            model.erase("physPreset");
            files::WriteText(projectRoot / "xmodel" / files::FromUtf8("skybox_" + project + ".json"), py::JsonDumps(model, 2) + "\n");
            return Json{{"materials", materials},
                        {"sky", Json{{"source_model", sourceName}, {"status", "source_geometry_and_paired_shaders"},
                                     {"materials", std::vector<std::string>(materialNames.begin(), materialNames.end())}}}};
        }
        catch (const StageError& error)
        {
            return Json{{"stage_error", error.what()}};
        }
    }
} // namespace w2b::pipeline
