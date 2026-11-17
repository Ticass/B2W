#pragma once

#include <array>
#include <map>
#include <span>
#include <vector>

namespace w2b::world
{
    struct ProbePlan
    {
        std::vector<int> kept;
        std::map<int, int> remap;
        int Target(int source) const;
    };

    ProbePlan PlanProbes(std::span<const std::array<double, 3>> origins, const std::map<int, int>& usage,
                         std::span<const int> baked, bool compensated);
}
