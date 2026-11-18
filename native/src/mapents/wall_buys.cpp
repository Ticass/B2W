#include "mapents/entities.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>

namespace w2b::mapents
{
    namespace
    {
        using Position = std::array<double, 3>;
        using Indices = std::vector<std::size_t>;
        using Groups = std::map<std::string, Indices, std::less<>>;

        void PairModels(Json& entities, std::string_view target, const Indices& triggers, Indices models, Groups& named, Json& renamed)
        {
            if (models.size() != triggers.size() || !std::ranges::all_of(models, [&](auto i) {
                    return Field(entities.at(i), "classname") == "script_model";
                }))
                throw std::invalid_argument("ambiguous wall-buy target " + std::string(target) + ": " +
                                            std::to_string(triggers.size()) + " triggers, " + std::to_string(models.size()) + " models");
            std::vector<Position> positions;
            positions.reserve(models.size());
            for (const auto model : models)
                positions.push_back(Origin(entities.at(model)));
            for (std::size_t i = 0; i < triggers.size(); ++i)
            {
                auto& trigger = entities.at(triggers[i]);
                const auto origin = Origin(trigger);
                std::size_t closest = 0;
                double distance = std::numeric_limits<double>::infinity();
                for (std::size_t m = 0; m < models.size(); ++m)
                {
                    const auto& position = positions[m];
                    const auto x = position[0] - origin[0], y = position[1] - origin[1], z = position[2] - origin[2];
                    if (const auto candidate = x * x + y * y + z * z; candidate < distance)
                    {
                        closest = m;
                        distance = candidate;
                    }
                }
                const auto name = "waw2bo2_wallbuy_" + std::string(target) + "_" + std::to_string(i);
                trigger["target"] = name;
                const auto model = models[closest];
                entities.at(model)["targetname"] = name;
                auto& paired = named[name];
                paired.insert(std::ranges::lower_bound(paired, model), model);
                models.erase(models.begin() + static_cast<Indices::difference_type>(closest));
                positions.erase(positions.begin() + static_cast<std::vector<Position>::difference_type>(closest));
                Increment(renamed, "wall buy target " + std::string(target) + " uniquely paired");
            }
        }
    }

    void NormalizeWallBuys(Json& entities, Json& renamed)
    {
        Groups named, buys;
        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            const auto& entity = entities.at(i);
            const auto name = Field(entity, "targetname");
            if (!name.empty())
                named[std::string(name)].push_back(i);
        }
        const auto triggers = named.find("weapon_upgrade");
        if (triggers == named.end())
            return;
        for (const auto i : triggers->second)
        {
            auto& entity = entities.at(i);
            if (const auto target = Field(entity, "target"); !target.empty())
                buys[std::string(target)].push_back(i);
            else
            {
                auto name = "waw_wallbuy_without_model_" + std::to_string(i);
                while (named.contains(name))
                    name += '_';
                entity["target"] = name;
                Increment(renamed, "WaW wall buy without display model -> unmatched T6 target");
            }
        }
        for (const auto& [target, group] : buys)
        {
            const auto matches = named.find(target);
            if (matches == named.end() || matches->second.size() <= 1)
                continue;
            auto models = std::move(matches->second);
            named.erase(matches);
            PairModels(entities, target, group, std::move(models), named, renamed);
        }
    }
}
