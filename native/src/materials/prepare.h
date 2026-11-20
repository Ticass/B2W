#pragma once

#include "core/text.h"
#include "materials/t6_material.h"
#include "materials/techsets.h"
#include "materials/waw_material.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace w2b::materials
{
    // the stock donor materials by technique set (parsed once, shared by threads)
    class Donors
    {
    public:
        explicit Donors(const util::Json& paths);
        const T6Material& Get(const std::string& name);
        std::vector<Technique> candidates;

    private:
        std::map<std::string, std::filesystem::path> paths_;
        std::map<std::string, T6Material> cached_;
        std::mutex lock_;
    };

    struct MaterialSelection
    {
        std::optional<std::string> error; // no donor fits
        std::string target;               // the donor technique set
        long long cost = 0;
        std::vector<std::string> matchNotes, notes;
        T6Material material;
        std::optional<std::string> approximation; // the WaW technique it is drawn as instead
    };

    // t6bridge._choose_waw_material: donor selection and material preparation
    MaterialSelection SelectMaterial(const WawMaterial& input, bool programs, bool layered, Donors& donors);
}
