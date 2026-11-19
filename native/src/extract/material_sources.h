#pragma once
// The WaW materials behind the converter's source roots, as objects.
//
// A root made by ExtractZone holds a zone's dump; its materials are read from
// that zone, loaded in process (oat::ReadMaterial), and the receipt's listing
// says which materials the zone defines, so finding one loads nothing. A
// root without a zone (a WaW Mod Tools compile) keeps its material files.
// Lookups follow the roots' order, as the dump files were searched.

#include "materials/waw_material.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::extract
{
    namespace fs = std::filesystem;

    class MaterialSources
    {
    public:
        explicit MaterialSources(const std::vector<fs::path>& roots);

        // whether a root defines the material
        bool Has(const std::string& name) const;
        // the material of the first root defining it
        std::optional<materials::WawMaterial> Find(const std::string& name) const;
        // reads these materials zone by zone, each zone loaded once, so the
        // lookups that follow never reload a zone another one pushed out
        void Read(const std::vector<std::string>& names) const;
        // the generated ("*a_b") materials the roots define
        std::set<std::string> GeneratedNames() const;

    private:
        struct Root
        {
            fs::path folder;
            std::optional<fs::path> fastfile;            // the zone it was dumped from
            std::map<std::string, std::string> materials; // key -> the zone's name
        };

        // the root defining the material and the name it goes by there
        std::optional<std::pair<const Root*, std::string>> Locate(const std::string& name) const;
        static materials::WawMaterial ReadFrom(const Root& root, const std::string& name);

        std::vector<Root> roots_;
        mutable std::mutex lock_;
        mutable std::map<std::string, materials::WawMaterial> read_; // by key
    };
} // namespace w2b::extract
