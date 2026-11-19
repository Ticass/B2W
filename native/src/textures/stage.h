#pragma once

#include "core/pytext.h"
#include "textures/iwi.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w2b::textures
{
    py::Json HeaderJson(const IwiHeader& header);
    IwiHeader ConvertFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                          bool faceMajor = false, bool baseOnly = false);

    // the world surfaces drawn with one WaW lightmap page
    struct SurfacePage
    {
        unsigned index = 0;
        std::size_t surfaces = 0;
        std::set<std::string> materials;
    };

    struct LightmapRequest
    {
        std::filesystem::path project;
        std::vector<std::filesystem::path> imageRoots;
        std::set<std::string> wawMaterials; // materials that read the WaW half of each page
        std::vector<SurfacePage> surfacePages;
        bool diagnostic = false;
        unsigned workers = 1;
    };

    // a shared dual-encoding page: T6 on the left, WaW on the right
    struct LightmapPage
    {
        std::size_t index = 0;
        unsigned width = 0, height = 0;
        std::string name;
        py::Json ToJson() const;
    };

    struct LightmapStage
    {
        std::vector<LightmapPage> pages;
        std::vector<std::string> warnings;
    };

    LightmapStage StageLightmaps(const LightmapRequest& request);
}
