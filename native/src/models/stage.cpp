#include "models/models.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "core/parallel.h"
#include "weapons/stage.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <windows.h>

namespace w2b::models
{
    namespace
    {
        using Names = std::set<std::string, std::less<>>;

        struct Lod
        {
            Json definition;
            Path source;
        };

        struct Job
        {
            std::string name;
            Path destination;
            Json model;
            std::vector<Lod> lods;
        };

        struct Result
        {
            std::vector<std::string> errors, warnings;
            Json hidden = Json::array();
            Names materials;
            std::optional<Bounds> bounds;
            bool collisionBox = false;
        };

        class ShadowMaterials
        {
        public:
            explicit ShadowMaterials(const std::vector<Path>& roots) : roots_(roots) {}

            bool OnlyShadow(const std::string& name)
            {
                {
                    const std::scoped_lock lock(mutex_);
                    if (const auto cached = flags_.find(name); cached != flags_.end())
                        return cached->second;
                }
                const auto file = weapons::FindSource(roots_, assets::MaterialPath(name));
                bool onlyShadow = false;
                if (!file.empty())
                {
                    const auto material = weapons::ReadAssetJson(file);
                    if (const auto flags = material.find("gameFlags"); flags != material.end())
                        onlyShadow = std::ranges::find(*flags, Json("1")) != flags->end();
                }
                const std::scoped_lock lock(mutex_);
                return flags_.try_emplace(name, onlyShadow).first->second;
            }

        private:
            const std::vector<Path>& roots_;
            std::mutex mutex_;
            std::map<std::string, bool, std::less<>> flags_;
        };

        class ModelStager
        {
        public:
            ModelStager(std::vector<Path> roots, Path project, Names boxModels)
                : roots_(std::move(roots)), project_(std::move(project)), boxModels_(std::move(boxModels)), shadows_(roots_) {}

            Job Plan(const std::string& name)
            {
                weapons::OutputName("xmodel", name);
                const auto relative = Path("xmodel") / files::FromUtf8(name + ".json");
                Job job{name, project_ / relative, {}, {}};
                const auto owner = std::ranges::find_if(roots_, [&](const Path& root) { return std::filesystem::exists(root / relative); });
                if (owner == roots_.end())
                    return job;
                job.model = weapons::ReadAssetJson(*owner / relative);
                if (const auto lods = job.model.find("lods"); lods != job.model.end())
                    for (const auto& lod : *lods)
                        job.lods.push_back({lod, weapons::FindModelLod(roots_, *owner, lod.at("file").get_ref<const std::string&>())});
                return job;
            }

            Result Convert(Job& job)
            {
                Result result;
                if (job.model.is_null())
                {
                    result.errors.emplace_back("xmodel " + job.name + ": xmodel/" + job.name + ".json missing from every WaW zone dump");
                    return result;
                }
                auto lods = Json::array();
                for (const auto& lod : job.lods)
                {
                    const auto& file = lod.definition.at("file").get_ref<const std::string&>();
                    if (lod.source.empty())
                    {
                        result.errors.emplace_back("xmodel " + job.name + ": LOD file " + file + " missing");
                        continue;
                    }
                    auto geometry = weapons::ReadAssetJson(lod.source);
                    if (lods.empty())
                        result.bounds = GameBounds(geometry);
                    WriteGeometry(geometry, lod, job.model, job.name, result);
                    lods.push_back(lod.definition);
                }
                if (!lods.empty())
                {
                    UpdateModel(job, std::move(lods), result);
                    files::WriteText(job.destination, util::JsonDumps(job.model, 4) + '\n');
                }
                return result;
            }

        private:
            void WriteGeometry(Json& geometry, const Lod& lod, const Json& model, const std::string& name, Result& result)
            {
                const auto& file = lod.definition.at("file").get_ref<const std::string&>();
                const auto kind = model.value("type", Json());
                const bool stripped = StripBadSkins(geometry, kind != "animated" && kind != "viewhands");
                if (stripped)
                    result.warnings.emplace_back("xmodel " + name + ": skin without a single root joint removed (" + file +
                                                 "); drawn rigid in its bind pose");
                const auto fixed = NeutralizeTransforms(geometry);
                if (fixed != 0)
                    result.warnings.emplace_back("xmodel " + name + ": " + std::to_string(fixed) +
                                                 " node transform(s) with NaN components neutralized (" + file + ")");
                auto hidden = DropShadowSurfaces(geometry, [this](const std::string& material) { return shadows_.OnlyShadow(material); });
                const auto destination = project_ / files::FromUtf8(file);
                if (stripped || fixed != 0 || !hidden.empty())
                    files::WriteText(destination, util::JsonDumpsDefault(geometry));
                else
                    weapons::CopySource(lod.source, destination);
                if (!hidden.empty())
                    result.hidden.push_back({{"model", name}, {"lod", file}, {"materials", std::move(hidden)}});
                if (const auto materials = geometry.find("materials"); materials != geometry.end())
                    for (const auto& material : *materials)
                        result.materials.insert(material.at("name").get_ref<const std::string&>());
            }

            void UpdateModel(Job& job, Json lods, Result& result) const
            {
                auto& model = job.model;
                model["_game"] = "t6";
                if (const auto bones = RepairHitboxes(model); bones != 0)
                    result.warnings.emplace_back("xmodel " + job.name + ": " + std::to_string(bones) +
                                                 " bone hitbox(es) with NaN or infinite values use their own box");
                model["lods"] = std::move(lods);
                const auto authored = model.find("collSurfs");
                if (boxModels_.contains(job.name) && (authored == model.end() || authored->empty() || *authored == false || *authored == 0))
                {
                    if (result.bounds)
                    {
                        model["collSurfs"] = Json::array({CollisionBox(*result.bounds, model.value("rootBoneName", Json("tag_origin")))});
                        model["contents"] = model.value("contents", std::int64_t{}) | ScriptModelContents;
                        model["collLod"] = 0;
                        result.collisionBox = true;
                    }
                    else
                        result.errors.emplace_back("xmodel " + job.name + ": no LOD0 positions for its script_model collision box");
                }
                const auto kind = model.value("type", Json());
                model["flags"] = kind == "animated" || kind == "viewhands" ? 0x80000 : 0x200000;
                model["lightingOriginOffset"] = {{"x", 0.0}, {"y", 0.0}, {"z", 0.5}};
                model["lightingOriginRange"] = 0.5;
                if (const auto preset = model.find("physPreset"); preset != model.end())
                {
                    if (!preset->is_null())
                        result.warnings.emplace_back("xmodel " + job.name + ": physPreset dropped (static models do not simulate)");
                    model.erase(preset);
                }
            }

            std::vector<Path> roots_;
            Path project_;
            Names boxModels_;
            ShadowMaterials shadows_;
        };

        struct PathOrder
        {
            bool operator()(const Path& a, const Path& b) const
            {
                return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            }
        };

        bool OverlappingOutputs(const std::vector<Job>& jobs, const Path& project)
        {
            // Some models share a destination LOD. Retain the source model order
            // for those inputs, including paths that differ only by case.
            std::map<Path, std::size_t, PathOrder> owners;
            const auto key = [](const Path& path) { return std::filesystem::absolute(path).lexically_normal().make_preferred(); };
            const auto reserve = [&](const Path& path, std::size_t i) {
                const auto [entry, _] = owners.try_emplace(key(path), i);
                return entry->second != i;
            };
            for (std::size_t i = 0; i < jobs.size(); ++i)
            {
                if (reserve(jobs[i].destination, i))
                    return true;
                for (const auto& lod : jobs[i].lods)
                    if (!lod.source.empty() && reserve(project / files::FromUtf8(lod.definition.at("file").get_ref<const std::string&>()), i))
                        return true;
            }
            for (std::size_t i = 0; i < jobs.size(); ++i)
                for (const auto& lod : jobs[i].lods)
                    if (!lod.source.empty())
                        if (const auto writer = owners.find(key(lod.source)); writer != owners.end() && writer->second != i)
                            return true;
            return false;
        }
    }

    Json StageModels(const Path& project, const Path& csv, std::vector<Path> roots, Json placements, Names names, Names boxModels)
    {
        for (const auto& placement : placements)
            names.insert(placement.at("name").get_ref<const std::string&>());
        ModelStager stage(std::move(roots), project, std::move(boxModels));
        std::vector<Job> jobs;
        jobs.reserve(names.size());
        for (const auto& name : names)
            jobs.push_back(stage.Plan(name));
        std::vector<Result> results(jobs.size());
        const auto workers = OverlappingOutputs(jobs, project) ? 1u : std::min(4u, parallel::Workers());
        parallel::For(jobs.size(), [&](std::size_t i) { results[i] = stage.Convert(jobs[i]); }, workers);

        Names materials;
        std::map<std::string_view, Bounds, std::less<>> bounds;
        auto content = Json::object();
        std::vector<std::string> warnings, errors;
        for (std::size_t i = 0; i < jobs.size(); ++i)
        {
            auto& result = results[i];
            if (result.bounds)
                bounds.emplace(jobs[i].name, std::move(*result.bounds));
            for (auto& entry : result.hidden)
                content["shadow_only_model_surfaces"].push_back(std::move(entry));
            if (result.collisionBox)
                content["script_model_collision_boxes"].push_back(jobs[i].name);
            materials.merge(result.materials);
            warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
            errors.insert(errors.end(), result.errors.begin(), result.errors.end());
        }
        if (std::filesystem::exists(csv))
            weapons::CopySource(csv, project / csv.filename());
        else
            errors.push_back(files::Utf8(csv) + " missing");
        for (auto& placement : placements)
            if (const auto model = bounds.find(placement.at("name").get_ref<const std::string&>()); model != bounds.end())
                placement["lightingOrigin"] = LightingOrigin(placement, model->second);
        files::WriteText(project / "BSP/models.json", util::JsonDumps({{"models", std::move(placements)}}, 1) + '\n');
        return {{"materials", std::move(materials)}, {"errors", std::move(errors)},
                {"warnings", std::move(warnings)}, {"content", std::move(content)}};
    }
}
