#pragma once

#include "core/pytext.h"
#include "materials/techsets.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace w2b::materials
{
    // the stock donor materials by technique set (parsed once, shared by threads)
    class Donors
    {
    public:
        explicit Donors(const py::Json& paths);
        const py::Json& Get(const std::string& name);
        std::vector<Technique> candidates;

    private:
        std::map<std::string, std::filesystem::path> paths_;
        std::map<std::string, py::Json> cached_;
        std::mutex lock_;
    };

    // t6bridge._choose_waw_material: donor selection and material preparation
    py::Json SelectMaterial(const py::Json& input, bool programs, bool layered, Donors& donors);
    void PrepareMaterials(const std::filesystem::path& request);
}
