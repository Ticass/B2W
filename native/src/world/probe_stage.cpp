#include "world/probe_stage.h"

#include "core/files.h"
#include "textures/stage.h"
#include "world/probe_plan.h"
#include "world/render_world.h"

#include <algorithm>
#include <numeric>
#include <set>

namespace w2b::world
{
    namespace
    {
        using py::Json;
        using files::fs::path;

        std::string ImageName(int probe)
        {
            return "reflection_probe" + std::to_string(probe);
        }

        path FindImage(const std::vector<path>& roots, std::string name)
        {
            std::ranges::replace(name, '*', '_');
            for (const auto& root : roots)
            {
                const auto file = root / files::FromUtf8(name + ".dds");
                if (files::fs::is_regular_file(file))
                    return file;
            }
            return {};
        }

        Json RemapManifest(const ProbePlan& plan)
        {
            auto remap = Json::object();
            for (const auto index : plan.kept)
                remap[std::to_string(index)] = plan.Target(index);
            for (const auto& [index, target] : plan.remap)
                if (!remap.contains(std::to_string(index)))
                    remap[std::to_string(index)] = target;
            return remap;
        }

        struct ProbeUsage
        {
            std::size_t staticModels;
            std::map<int, int> counts;
        };

        ProbeUsage WorldProbeUsage(const Json& request)
        {
            if (request.contains("surface_counts"))
            {
                ProbeUsage usage{request.at("static_model_count").get<std::size_t>(), {}};
                for (const auto& row : request.at("surface_counts"))
                    usage.counts[row.at(0).get<int>()] = row.at(1).get<int>();
                return usage;
            }
            const auto gfx = files::FromUtf8(request.at("gfx").get<std::string>());
            const auto world = ReadRenderWorld(gfx, request.at("compensated").get<bool>());
            ProbeUsage usage{world.staticModels.size(), {}};
            for (const auto& surface : world.surfaces)
                ++usage.counts[surface.reflectionProbe];
            return usage;
        }
    }

    py::Json StageProbes(const path& request)
    {
        const auto args = Json::parse(files::ReadText(request));
        const auto source = files::FromUtf8(args.at("source").get<std::string>());
        auto report = Json{{"warnings", Json::array()}, {"errors", Json::array()}, {"images", Json::array()}};
        if (!files::fs::is_regular_file(source))
        {
            report["warnings"].push_back("reflection probes: the render dump has none (re-extract gfxworld with the "
                                         "reflection-probe T4 dumper); specular reflections use the black neutral probe");
            return report;
        }
        const auto data = Json::parse(files::ReadText(source));
        auto usage = WorldProbeUsage(args);
        const auto staticProbes = data.value("smodelProbes", std::vector<int>{});
        if (!staticProbes.empty() && staticProbes.size() != usage.staticModels)
        {
            report["errors"].push_back("reflection probes: " + std::to_string(staticProbes.size()) +
                                       " static model probes for " + std::to_string(usage.staticModels) + " static models");
            return report;
        }

        std::vector<path> roots;
        for (const auto& root : args.at("image_roots"))
            roots.push_back(files::FromUtf8(root.get<std::string>()));
        std::vector<std::array<double, 3>> origins;
        std::vector<std::string> names;
        std::vector<path> images;
        std::vector<int> baked, missing;
        for (const auto& probe : data.at("probes"))
        {
            const auto index = static_cast<int>(names.size());
            origins.push_back(probe.at("origin").get<std::array<double, 3>>());
            auto name = probe.at("image").get<std::string>();
            if (name.starts_with(','))
                name.erase(0, 1);
            images.push_back(index && !name.empty() ? FindImage(roots, name) : path{});
            if (index && !name.empty())
                (images.back().empty() ? missing : baked).push_back(index);
            names.push_back(std::move(name));
        }
        if (!missing.empty())
        {
            std::string list;
            for (const auto i : missing)
                list += (list.empty() ? "" : ", ") + std::to_string(i);
            report["errors"].push_back("reflection probes [" + list + "]: cube images missing from every image root");
        }

        for (const auto probe : staticProbes)
            ++usage.counts[probe];
        const auto plan = PlanProbes(origins, usage.counts, baked, args.at("compensated").get<bool>());
        const auto project = files::FromUtf8(args.at("project").get<std::string>());
        auto entries = Json::array();
        for (std::size_t t6 = 0; t6 < plan.kept.size(); ++t6)
        {
            const auto index = plan.kept[t6];
            const auto name = ImageName(static_cast<int>(t6));
            entries.push_back({{"origin", static_cast<std::size_t>(index) < origins.size() ?
                data.at("probes").at(index).at("origin") : Json{0.0, 0.0, 0.0}}, {"image", name}});
            if (t6)
            {
                textures::ConvertFile(images[index], project / "images" / (name + ".iwi"));
                report["images"].push_back({{"name", name}, {"source", files::Utf8(images[index])},
                                            {"reflection_probe", names[index]}});
            }
        }
        std::vector<int> cellProbes(plan.kept.size() == 1 ? 1 : plan.kept.size() - 1);
        std::iota(cellProbes.begin(), cellProbes.end(), plan.kept.size() == 1 ? 0 : 1);
        files::WriteText(project / "BSP" / "reflectionprobes.json",
            py::JsonDumps({{"probes", std::move(entries)}, {"cellProbes", cellProbes}}, 1) + "\n");
        if (baked.size() > plan.kept.size() - 1)
            report["warnings"].push_back("reflection probes: T6 addresses 32 probes, the map bakes " +
                std::to_string(baked.size()) + "; kept " + std::to_string(plan.kept.size() - 1) +
                ", each dropped probe drawn with its nearest kept one");
        auto cells = Json::array();
        for (const auto& cell : data.value("cells", Json::array()))
        {
            std::set<int> remapped;
            for (const auto& probe : cell.at("probes"))
                if (const auto index = plan.Target(probe.get<int>()); index)
                    remapped.insert(index);
            cells.push_back(remapped);
        }
        report["content"] = {{"source", origins.size()}, {"baked", baked.size()}, {"t6", plan.kept.size()},
                             {"remap", RemapManifest(plan)}};
        report["kept"] = plan.kept;
        report["smodel_probes"] = staticProbes;
        report["cells"] = std::move(cells);
        return report;
    }
}
