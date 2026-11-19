#include "textures/stage.h"

#include "core/files.h"
#include "core/parallel.h"
#include "textures/lightmap.h"

#include <set>

namespace w2b::textures
{
    namespace
    {
        using py::Json;

        std::optional<files::fs::path> FindPage(const std::vector<files::fs::path>& roots, unsigned index,
                                               std::string_view kind)
        {
            const auto name = "_lightmap" + std::to_string(index) + '_' + std::string(kind) + ".dds";
            for (const auto& root : roots)
            {
                auto candidate = root / name;
                if (files::fs::exists(candidate))
                    return candidate;
            }
            return std::nullopt;
        }
    }

    py::Json LightmapPage::ToJson() const
    {
        return {{"index", index}, {"width", width}, {"height", height}, {"layout", "t6_left_waw_right"}, {"waw", name}, {"t6", name}};
    }

    LightmapStage StageLightmaps(const LightmapRequest& request)
    {
        const auto& project = request.project;
        const auto& roots = request.imageRoots;
        struct Page
        {
            files::fs::path secondary;
            std::optional<files::fs::path> primary;
        };
        std::vector<Page> pages;
        for (unsigned index = 0; auto secondary = FindPage(roots, index, "secondary"); ++index)
            pages.push_back({std::move(*secondary), FindPage(roots, index, "primary")});

        const auto& materials = request.wawMaterials;
        std::vector<std::string> warnings;
        std::size_t unlit = 0;
        std::vector<unsigned> missingIndices;
        std::set<std::string> litMaterials;
        for (const auto& surfacePage : request.surfacePages)
        {
            if (surfacePage.index >= pages.size())
            {
                unlit += surfacePage.surfaces;
                missingIndices.push_back(surfacePage.index);
            }
            else
                litMaterials.insert(surfacePage.materials.begin(), surfacePage.materials.end());
        }
        if (unlit)
        {
            std::string list;
            for (const auto index : missingIndices)
                list += (list.empty() ? "" : ", ") + std::to_string(index);
            warnings.push_back(std::to_string(unlit) + " surfaces have no lightmap page (WaW index >= " +
                               std::to_string(pages.size()) + ": [" + list + "])");
        }
        // The request is ordered by source page. Keep report and manifest order
        // independent of which conversion finishes first.
        std::vector<LightmapPage> reportPages(pages.size());
        std::vector<Json> manifestPages(pages.size());
        for (std::size_t index = 0; index < pages.size(); ++index)
            if (!pages[index].primary)
                warnings.push_back("lightmap page " + std::to_string(index) + ": primary (sun visibility) absent; fully visible");
        const bool diagnostic = request.diagnostic;
        parallel::For(pages.size(), [&](std::size_t index)
        {
            const auto& page = pages[index];
            const auto name = "lightmap" + std::to_string(index) + "_dual_secondary";
            const auto image = BuildLightmap(page.secondary, page.primary, static_cast<unsigned>(index), diagnostic);
            files::WriteBytes(project / "images" / (name + ".iwi"), image.bytes);
            manifestPages[index] = {{"waw", name}, {"t6", name}};
            reportPages[index] = {index, image.header.width, image.header.height / 3u, name};
        }, request.workers);

        if (!pages.empty())
        {
            warnings.push_back("lightmap sun visibility (WaW primary) box-filtered to the secondary page resolution");
            if (materials.size() < litMaterials.size())
                warnings.push_back("APPROXIMATED lightmaps for T6 donor materials: WaW flat-normal lighting, no "
                                   "directional term, hdrControl0.x assumed 1");
        }
        files::WriteText(project / "BSP" / "lightmaps.json",
                         py::JsonDumps({{"pages", std::move(manifestPages)}, {"wawMaterials", materials}}, 1) + "\n");
        return {std::move(reportPages), std::move(warnings)};
    }
}
