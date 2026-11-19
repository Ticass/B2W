#pragma once
// Port of fxmaterials.py: WaW effect materials on stock BO2 effect donors.

#include "core/pytext.h"
#include "materials/waw_material.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w2b::materials
{
    struct EffectDonor
    {
        std::string techset;
        std::filesystem::path path;
        py::Json material;
        std::set<std::string> features;
        std::string blend;
    };

    bool IsEffectTechset(const std::string& t4Techset);
    // index_effect_donors: every stock BO2 effect material usable as a donor
    std::vector<EffectDonor> IndexEffectDonors(const std::filesystem::path& stockMaterials, const std::filesystem::path* techsetDump, const std::filesystem::path& bo2Raw);
    // choose_donor: (donor, notes); throws TechniqueError when none fits
    std::pair<const EffectDonor*, std::vector<std::string>> ChooseEffectDonor(const WawMaterial& t4, const std::vector<EffectDonor>& donors);
    py::Json BuildEffectMaterial(const WawMaterial& t4, const EffectDonor& donor, std::vector<std::string>& notes);
} // namespace w2b::materials
