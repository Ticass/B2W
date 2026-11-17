#include "world/probe_plan.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace w2b::world
{
    namespace
    {
        constexpr std::size_t room = 31;

        class Distances
        {
        public:
            Distances(std::span<const std::array<double, 3>> origins, std::span<const int> candidates)
                : origins_(origins), candidates_(candidates)
            {
                if (candidates.size() > 512)
                    return;
                cached_.resize(candidates.size() * candidates.size());
                for (std::size_t a = 0; a < candidates.size(); ++a)
                    for (std::size_t b = a + 1; b < candidates.size(); ++b)
                        cached_[a * candidates.size() + b] = cached_[b * candidates.size() + a] = Measure(a, b);
            }

            double Get(std::size_t a, std::size_t b) const
            {
                return cached_.empty() ? Measure(a, b) : cached_[a * candidates_.size() + b];
            }

        private:
            double Measure(std::size_t a, std::size_t b) const
            {
                const auto& p = origins_[candidates_[a]];
                const auto& q = origins_[candidates_[b]];
                return std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
            }

            std::span<const std::array<double, 3>> origins_;
            std::span<const int> candidates_;
            std::vector<double> cached_;
        };

        std::size_t Nearest(std::size_t probe, std::span<const std::size_t> choices, const Distances& distances)
        {
            return *std::ranges::min_element(choices, {}, [&](std::size_t k)
            {
                return std::pair{distances.Get(probe, k), k};
            });
        }

        double WeightedCost(std::span<const std::size_t> group, std::size_t candidate,
                            const std::vector<int>& weights, const Distances& distances, bool compensated)
        {
            double sum = 0, correction = 0;
            for (const auto member : group)
            {
                const auto cost = weights[member] * distances.Get(member, candidate);
                const auto total = sum + cost;
                if (compensated)
                    correction += (std::max(sum, cost) - total) + std::min(sum, cost);
                sum = total;
            }
            return sum + correction;
        }

        std::vector<std::size_t> SelectProbes(const std::vector<int>& weights, const Distances& distances, bool compensated)
        {
            std::vector<std::size_t> kept;
            kept.push_back(static_cast<std::size_t>(std::ranges::max_element(weights) - weights.begin()));
            std::vector<bool> selected(weights.size());
            selected[kept.front()] = true;
            while (kept.size() < room)
            {
                double best = -1.0;
                std::size_t next = 0;
                for (std::size_t i = 0; i < weights.size(); ++i)
                {
                    if (selected[i])
                        continue;
                    const auto score = weights[i] * distances.Get(i, Nearest(i, kept, distances));
                    if (score > best)
                    {
                        best = score;
                        next = i;
                    }
                }
                selected[next] = true;
                kept.push_back(next);
            }
            std::ranges::sort(kept);
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                std::vector<std::vector<std::size_t>> groups(weights.size());
                for (std::size_t i = 0; i < weights.size(); ++i)
                    groups[Nearest(i, kept, distances)].push_back(i);
                std::vector<std::size_t> moved;
                for (const auto medoid : kept)
                {
                    const auto& group = groups[medoid];
                    if (group.empty())
                        throw std::runtime_error("reflection probe cluster has no members");
                    moved.push_back(*std::ranges::min_element(group, {}, [&](std::size_t candidate)
                    {
                        return std::pair{WeightedCost(group, candidate, weights, distances, compensated), candidate};
                    }));
                }
                std::ranges::sort(moved);
                if (moved == kept)
                    break;
                kept = std::move(moved);
            }
            return kept;
        }
    }

    int ProbePlan::Target(int source) const
    {
        const auto found = remap.find(source);
        return found == remap.end() ? 0 : found->second;
    }

    ProbePlan PlanProbes(std::span<const std::array<double, 3>> origins, const std::map<int, int>& usage,
                         std::span<const int> baked, bool compensated)
    {
        std::vector<int> candidates;
        for (const auto id : baked)
            if (id > 0 && static_cast<std::size_t>(id) < origins.size())
                candidates.push_back(id);
        std::ranges::sort(candidates);
        ProbePlan plan{{0}, {{0, 0}}};
        if (candidates.size() <= room)
        {
            for (const auto probe : candidates)
            {
                plan.remap.emplace(probe, static_cast<int>(plan.kept.size()));
                plan.kept.push_back(probe);
            }
            return plan;
        }

        std::vector<int> weights;
        for (const auto probe : candidates)
        {
            const auto count = usage.find(probe);
            weights.push_back(count == usage.end() ? 1 : std::max(1, count->second));
        }
        const Distances distances(origins, candidates);
        const auto kept = SelectProbes(weights, distances, compensated);
        for (const auto index : kept)
        {
            const auto probe = candidates[index];
            plan.remap.emplace(probe, static_cast<int>(plan.kept.size()));
            plan.kept.push_back(probe);
        }
        for (std::size_t i = 0; i < candidates.size(); ++i)
            if (!plan.remap.contains(candidates[i]))
                plan.remap.emplace(candidates[i], plan.Target(candidates[Nearest(i, kept, distances)]));
        return plan;
    }
}
