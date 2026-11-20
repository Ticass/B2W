#include "effects/stage.h"

#include "core/files.h"
#include "core/parallel.h"
#include "effects/effect.h"

#include <algorithm>
#include <iterator>
#include <map>

namespace w2b::effects
{
    namespace
    {
        using util::Json;
        using Names = std::set<std::string>;

        struct StagedEffect
        {
            std::string name;
            Dependencies deps;
            std::string outputName;
            std::string serialized;
            std::vector<std::string> notes;
            std::string failure;
        };

        std::string NameList(const Names& names)
        {
            std::string result = "[";
            for (const auto& name : names)
            {
                if (result.size() > 1)
                    result += ", ";
                result += util::StringRepr(name);
            }
            return result + "]";
        }

        StagedEffect Prepare(const std::pair<std::string, files::fs::path>& entry, const MaterialNames& materials, const Names& failedMaterials)
        {
            StagedEffect effect;
            effect.name = entry.first;
            const auto source = Json::parse(files::ReadText(entry.second));
            effect.deps = FindDependencies(source);
            Names missing;
            std::ranges::set_intersection(effect.deps.materials, failedMaterials, std::inserter(missing, missing.end()));
            if (!missing.empty())
            {
                effect.failure = "materials " + NameList(missing) + " not converted";
                return effect;
            }
            try
            {
                auto converted = ConvertEffect(source, materials);
                effect.outputName = converted.asset.at("name").get<std::string>();
                effect.serialized = util::JsonDumps(converted.asset, 1) + "\n";
                effect.notes = std::move(converted.notes);
            }
            catch (const EffectError& error)
            {
                effect.failure = error.what();
            }
            return effect;
        }

        void RejectBrokenLinks(std::vector<StagedEffect>& staged, std::map<std::string, std::string>& failures,
                               const Names& absent)
        {
            for (bool changed = true; changed;)
            {
                changed = false;
                for (auto& effect : staged)
                {
                    if (!effect.failure.empty())
                        continue;
                    Names broken;
                    for (const auto& name : effect.deps.effects)
                        if (failures.contains(name) || absent.contains(name))
                            broken.insert(name);
                    if (broken.empty())
                        continue;
                    effect.failure = "references effects that are not converted: " + NameList(broken);
                    failures.emplace(effect.name, effect.failure);
                    changed = true;
                }
            }
        }
    }

    EffectConversion StageEffects(const EffectRequest& request)
    {
        const auto& output = request.output;
        const auto& entries = request.effects;
        std::vector<StagedEffect> staged(entries.size());
        parallel::For(staged.size(), [&](std::size_t i)
        {
            staged[i] = Prepare(entries[i], request.materialNames, request.failedMaterials);
        }, request.workers);

        Names sounds, models;
        std::vector<std::string> warnings;
        std::map<std::string, std::string> failures;
        for (const auto& effect : staged)
        {
            sounds.insert(effect.deps.sounds.begin(), effect.deps.sounds.end());
            if (!effect.failure.empty())
            {
                failures.emplace(effect.name, effect.failure);
                continue;
            }
            models.insert(effect.deps.models.begin(), effect.deps.models.end());
            for (const auto& note : effect.notes)
                warnings.push_back("fx " + effect.name + ": " + note);
        }
        RejectBrokenLinks(staged, failures, request.absent);
        for (const auto& [name, reason] : failures)
            warnings.push_back("UNSUPPORTED_FX " + name + ": not converted: " + reason);

        Names converted;
        for (const auto& effect : staged)
        {
            if (!effect.failure.empty())
                continue;
            files::WriteText(output / "fx" / files::FromUtf8(effect.outputName + ".w2bfx.json"), effect.serialized);
            converted.insert(effect.name);
        }
        return {std::move(converted), failures.size(), std::move(warnings), std::move(models), std::move(sounds)};
    }
}
