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

    py::Json StageLightmaps(const files::fs::path& request)
    {
        const auto args = Json::parse(files::ReadText(request));
        const auto project = files::FromUtf8(args.at("project").get<std::string>());
        std::vector<files::fs::path> roots;
        for (const auto& root : args.at("image_roots"))
            roots.push_back(files::FromUtf8(root.get<std::string>()));
        struct Page
        {
            files::fs::path secondary;
            std::optional<files::fs::path> primary;
        };
        std::vector<Page> pages;
        for (unsigned index = 0; auto secondary = FindPage(roots, index, "secondary"); ++index)
            pages.push_back({std::move(*secondary), FindPage(roots, index, "primary")});

        const auto materials = args.at("waw_materials").get<std::set<std::string>>();
        auto warnings = Json::array();
        std::size_t unlit = 0;
        std::vector<unsigned> missingIndices;
        std::set<std::string> litMaterials;
        for (const auto& surfacePage : args.at("surface_pages"))
        {
            const auto index = surfacePage.at(0).get<unsigned>();
            if (index >= pages.size())
            {
                unlit += surfacePage.at(1).get<std::size_t>();
                missingIndices.push_back(index);
            }
            else
                for (const auto& material : surfacePage.at(2))
                    litMaterials.insert(material.get<std::string>());
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
        std::vector<Json> reportPages(pages.size()), manifestPages(pages.size());
        for (std::size_t index = 0; index < pages.size(); ++index)
            if (!pages[index].primary)
                warnings.push_back("lightmap page " + std::to_string(index) + ": primary (sun visibility) absent; fully visible");
        const bool diagnostic = args.at("diagnostic").get<bool>();
        parallel::For(pages.size(), [&](std::size_t index)
        {
            const auto& page = pages[index];
            const auto name = "lightmap" + std::to_string(index) + "_dual_secondary";
            const auto image = BuildLightmap(page.secondary, page.primary, static_cast<unsigned>(index), diagnostic);
            files::WriteBytes(project / "images" / (name + ".iwi"), image.bytes);
            manifestPages[index] = {{"waw", name}, {"t6", name}};
            reportPages[index] = {{"index", index}, {"width", image.header.width}, {"height", image.header.height / 3},
                                  {"layout", "t6_left_waw_right"}, {"waw", name}, {"t6", name}};
        }, args.at("workers").get<unsigned>());

        if (!pages.empty())
        {
            warnings.push_back("lightmap sun visibility (WaW primary) box-filtered to the secondary page resolution");
            if (materials.size() < litMaterials.size())
                warnings.push_back("APPROXIMATED lightmaps for T6 donor materials: WaW flat-normal lighting, no "
                                   "directional term, hdrControl0.x assumed 1");
        }
        files::WriteText(project / "BSP" / "lightmaps.json",
                         py::JsonDumps({{"pages", std::move(manifestPages)}, {"wawMaterials", materials}}, 1) + "\n");
        return {{"pages", std::move(reportPages)}, {"warnings", std::move(warnings)}, {"errors", Json::array()}};
    }
}
