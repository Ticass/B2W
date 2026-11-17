#pragma once

#include "weapons/stage.h"

namespace w2b::weapons
{
    struct WeaponSource
    {
        std::string name;
        path file;
    };

    struct Plan
    {
        std::vector<WeaponSource> sources;
        std::vector<Weapon> weapons;
        Json report;
    };

    std::vector<WeaponSource> Discover(const std::vector<path>& roots);
    Plan PlanWeapons(const std::vector<path>& roots, const path& project, const path& base);
}
