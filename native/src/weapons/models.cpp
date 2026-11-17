#include "weapons/stage.h"

#include "core/encoding.h"
#include "core/files.h"
#include "weapons/skeleton.h"

#include <algorithm>

namespace w2b::weapons
{
    namespace
    {
        constexpr std::size_t AnimatedLodVertices = 32767;

        struct Lod
        {
            Json metadata;
            path destination;
            Json geometry;
        };

        struct ModelReport
        {
            Json models = Json::array();
            Json missing = Json::array();
            Json unsupported = Json::object();
            Names materials;
            Json presets = Json::object();
            Json reparented = Json::object();
            Json rebound = Json::array();
            Json strippedCollision = Json::array();
        };

        bool Truthy(const Json& value)
        {
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_number())
                return value != 0;
            if (value.is_string())
                return !value.get_ref<const std::string&>().empty();
            return !value.empty();
        }

        std::string StagePreset(const std::vector<path>& roots, const path& project, const std::string& name, Json& report)
        {
            const auto renamed = OutputName("physpreset", name);
            if (report.contains(name))
                return renamed;
            const auto source = FindSource(roots, path("physic") / files::FromUtf8(name));
            std::string bytes;
            if (source.empty())
                throw WeaponError("physics preset " + name + ": physic/" + name + " absent from the supplied WaW dumps");
            if (!files::ReadBytes(source, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(source));
            const auto text = encoding::WawText(bytes);
            const auto parts = py::Split(text, "\\");
            if (parts.front() != "PHYSIC" || parts.size() % 2 != 1)
                throw WeaponError("physics preset " + name + ": not a PHYSIC infostring");
            auto fields = Json::object();
            for (std::size_t i = 1; i < parts.size(); i += 2)
                fields[parts[i]] = parts[i + 1];
            if (const auto& prefix = Field(fields, "sndAliasPrefix"); !prefix.empty())
                fields["sndAliasPrefix"] = "waw/" + prefix;
            files::WriteText(project / "physic" / files::FromUtf8(renamed), WriteInfo(fields, "PHYSIC"));
            report[name] = {{"name", renamed}, {"source", files::Utf8(source)}};
            return renamed;
        }

        const Json& Required(const Json& object, std::string_view key)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
                throw WeaponError(py::StringRepr(key));
            return *entry;
        }

        std::vector<Lod> PrepareLods(const Json& model, const std::vector<path>& roots, const path& owner,
                                    const std::string& name, const std::string& renamed, Names& materials, ModelReport& report)
        {
            std::vector<Lod> lods;
            const auto list = model.find("lods");
            if (list == model.end() || list->empty())
                throw WeaponError("xmodel has no LODs");
            for (const auto& lod : *list)
            {
                const auto& file = Required(lod, "file").get_ref<const std::string&>();
                const auto source = FindModelLod(roots, owner, file);
                if (source.empty())
                    throw WeaponError("model geometry " + file + " is absent from the map and resolved WaW stock assets");
                auto geometry = ReadAssetJson(source);
                auto merged = PrepareSkeleton(geometry, Field(model, "type") == "rigid");
                if (!merged.empty())
                    report.reparented[name] = std::move(merged);
                if (const auto buffers = geometry.find("buffers"); buffers != geometry.end())
                    for (const auto& buffer : *buffers)
                    {
                        const auto uri = buffer.find("uri");
                        if (uri == buffer.end() || !Truthy(*uri))
                            continue;
                        const auto& value = uri->get_ref<const std::string&>();
                        if (!value.starts_with("data:"))
                            throw WeaponError("external GLTF resource needs dependency staging: " + value);
                    }
                if (const auto definitions = geometry.find("materials"); definitions != geometry.end())
                    for (auto& material : *definitions)
                    {
                        auto original = Required(material, "name").get<std::string>();
                        material["name"] = OutputName("material", original);
                        report.materials.insert(original);
                        materials.insert(std::move(original));
                    }
                auto destination = path("model_export") / files::FromUtf8(renamed + "_lod" + std::to_string(lods.size()) + ".gltf");
                auto metadata = lod;
                metadata["file"] = PosixPath(destination);
                lods.push_back({std::move(metadata), std::move(destination), std::move(geometry)});
            }
            return lods;
        }

        bool RebindOversizedLods(std::vector<Lod>& lods)
        {
            if (!std::ranges::any_of(lods, [](const Lod& lod) { return LodVertices(lod.geometry) > AnimatedLodVertices; }))
                return false;
            bool rebound = true;
            for (auto& lod : lods)
                if (!RebindToRoot(lod.geometry))
                    rebound = false;
            return rebound;
        }

        void StageModel(const std::vector<path>& roots, const path& project, const std::string& name,
                        const Names& worldModels, ModelReport& report)
        {
            const auto renamed = OutputName("xmodel", name);
            const auto relative = path("xmodel") / files::FromUtf8(name + ".json");
            const auto owner = std::ranges::find_if(roots, [&](const path& root) {
                return std::filesystem::is_regular_file(root / relative);
            });
            if (owner == roots.end())
            {
                report.missing.push_back(name);
                return;
            }
            const auto source = *owner / relative;
            auto model = ReadAssetJson(source);
            Names materials;
            std::vector<Lod> lods;
            try
            {
                if (const auto flags = model.find("flags"); flags != model.end() && Truthy(*flags))
                    throw WeaponError("nonzero T4 xmodel flags need measured T6 translation");
                if (const auto preset = model.find("physPreset"); preset != model.end() && Truthy(*preset))
                    model["physPreset"] = StagePreset(roots, project, preset->get_ref<const std::string&>(), report.presets);
                lods = PrepareLods(model, roots, *owner, name, renamed, materials, report);
                if (worldModels.contains(renamed) && Field(model, "type") == "animated" && RebindOversizedLods(lods))
                {
                    model["type"] = "rigid";
                    report.rebound.push_back(name);
                }
            }
            catch (const std::runtime_error& error)
            {
                report.unsupported[name] = error.what();
                return;
            }
            catch (const Json::exception& error)
            {
                report.unsupported[name] = error.what();
                return;
            }

            auto metadata = Json::array();
            for (auto& lod : lods)
            {
                files::WriteBytes(project / lod.destination, py::JsonDumps(lod.geometry, -1));
                metadata.push_back(std::move(lod.metadata));
            }
            model["_game"] = "t6";
            if (const auto collision = model.find("collSurfs"); collision != model.end())
            {
                if (Truthy(*collision))
                    report.strippedCollision.push_back(name);
                model.erase(collision);
            }
            model.erase("contents");
            if (const auto boneInfo = model.find("boneInfo"); boneInfo != model.end())
            {
                auto original = std::move(*boneInfo);
                model.erase(boneInfo);
                model["wawBoneInfo"] = std::move(original);
            }
            model["lods"] = std::move(metadata);
            model["lightingOriginOffset"] = {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}};
            model["lightingOriginRange"] = 0.0;
            files::WriteText(project / "xmodel" / files::FromUtf8(renamed + ".json"), py::JsonDumps(model, 2) + "\n");
            report.models.push_back({{"name", renamed}, {"waw", name}, {"source", files::Utf8(source)},
                                     {"lods", lods.size()}, {"materials", materials}});
        }
    }

    Json StageModels(const path& request)
    {
        const auto inputs = ReadAssetJson(request);
        const auto project = files::FromUtf8(inputs.at("project").get_ref<const std::string&>());
        std::vector<path> roots;
        for (const auto& root : inputs.at("roots"))
            roots.push_back(files::FromUtf8(root.get_ref<const std::string&>()));
        const auto names = inputs.at("names").get<Names>();
        const auto worldModels = inputs.at("worldModels").get<Names>();
        ModelReport report;
        try
        {
            for (const auto& name : names)
                StageModel(roots, project, name, worldModels, report);
        }
        catch (const WeaponError& error)
        {
            return {{"error", error.what()}};
        }
        return {{"status", "source_models_staged_materials_not_translated"}, {"models", std::move(report.models)},
                {"missing", std::move(report.missing)}, {"unsupported", std::move(report.unsupported)},
                {"materials", report.materials}, {"physpresets", std::move(report.presets)},
                {"reparented_roots", std::move(report.reparented)}, {"rebound_to_root", std::move(report.rebound)},
                {"collision_not_carried", std::move(report.strippedCollision)}};
    }
}
