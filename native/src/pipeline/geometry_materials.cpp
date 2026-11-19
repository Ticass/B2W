#include "pipeline/geometry_materials.h"

#include "assets/resolver.h"
#include "core/asset_paths.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "materials/techsets.h"
#include "pipeline/bridge_support.h"
#include "world/layers.h"
#include "world/render_world.h"

#include <algorithm>
#include <map>

namespace w2b::pipeline
{
    namespace
    {
        using py::Json;
        namespace fs = std::filesystem;
        constexpr int unknownFormat = 0xff;

        std::optional<Json> FindMaterial(const std::vector<fs::path>& roots, const std::string& name)
        {
            const auto relative = assets::MaterialPath(name);
            for (const auto& root : roots)
                if (fs::exists(root / relative))
                    return mod::Json(root / relative);
            return std::nullopt;
        }

        std::string TechniqueName(const Json& material)
        {
            const auto found = material.find("techniqueSet");
            return found != material.end() && found->is_string() ? found->get<std::string>() : "";
        }

        int VertexFormat(const materials::Technique& technique)
        {
            if (technique.layers.empty())
                return 0;
            const int coords = static_cast<int>(std::min<std::size_t>(technique.layers.size(), 5));
            const auto withNormals = std::ranges::count_if(technique.layers, [](const auto& layer) { return layer.features.contains('n'); });
            const auto normals = static_cast<int>(std::clamp<std::ptrdiff_t>(withNormals, 1, std::min(coords, 3)));
            for (int format = 0; format < 12; ++format)
            {
                const auto layout = world::LayerFormat(format);
                if (layout.extraUv + 1 == coords && layout.extraNormals + 1 == normals)
                    return format;
            }
            throw std::logic_error("missing world vertex format");
        }

        std::map<int, int> InferFormats(const world::RenderWorld& world)
        {
            std::map<std::int32_t, std::vector<const world::WorldSurface*>> byOffset;
            for (const auto& surface : world.surfaces)
                if (surface.layerFormat != 0)
                    byOffset[surface.layerOffset].push_back(&surface);
            std::map<int, int> formats;
            for (auto group = byOffset.begin(); group != byOffset.end(); ++group)
            {
                std::uint64_t begin = group->second.front()->firstVertex, end = begin;
                for (const auto* surface : group->second)
                {
                    begin = std::min<std::uint64_t>(begin, surface->firstVertex);
                    end = std::max(end, static_cast<std::uint64_t>(surface->firstVertex) + surface->vertexCount);
                }
                const auto next = std::next(group);
                const auto gap = (next == byOffset.end() ? static_cast<std::int64_t>(world.layerData.size()) : next->first) - group->first;
                const auto span = static_cast<std::int64_t>(end - begin);
                if (span == 0 || gap % span != 0)
                    continue;
                const auto stride = gap / span;
                int inferred = 0;
                for (int format = 1; format < 12; ++format)
                    if (world::LayerFormat(format).Stride() == stride)
                    {
                        if (inferred != 0)
                        {
                            inferred = 0;
                            break;
                        }
                        inferred = format;
                    }
                if (inferred != 0)
                    for (const auto* surface : group->second)
                        if (surface->layerFormat == unknownFormat)
                            formats.emplace(surface->index, inferred);
            }
            return formats;
        }

        std::vector<fs::path> DependencyRoots(const std::vector<fs::path>& roots, extract::StockWaw* stock, const std::set<std::string>& names)
        {
            assets::AssetSet requests;
            for (const auto& name : names)
                requests.emplace("material", name);
            assets::StockLookup lookup;
            if (stock)
                lookup = [stock](const assets::Asset& asset) { return stock->RootFor(asset.first, asset.second); };
            return assets::Expand(extract::ResourceRoot(), roots, requests, std::move(lookup)).roots;
        }

        void ResolveFormats(world::RenderWorld& world, const std::vector<fs::path>& roots, extract::StockWaw* stock, Json& warnings)
        {
            std::set<std::string> unknown;
            for (const auto& surface : world.surfaces)
                if (surface.layerFormat == unknownFormat)
                    unknown.insert(surface.material);
            if (unknown.empty() || world.layerData.empty())
                return;

            const auto resolvedRoots = DependencyRoots(roots, stock, unknown);
            std::map<std::string, int> resolved;
            for (const auto& name : unknown)
                if (auto material = FindMaterial(resolvedRoots, name))
                    resolved.emplace(name, VertexFormat(materials::ParseTechnique(TechniqueName(*material))));
            const auto inferred = InferFormats(world);
            std::set<std::int32_t> offsets;
            for (auto& surface : world.surfaces)
            {
                if (const auto material = resolved.find(surface.material); material != resolved.end())
                    surface.layerFormat = material->second;
                else if (const auto format = inferred.find(surface.index); format != inferred.end())
                    surface.layerFormat = format->second;
                if (surface.layerFormat != 0)
                    offsets.insert(surface.layerOffset);
            }

            // Three- and four-layer generated materials share a 24-byte stride.
            unknown.clear();
            for (auto& surface : world.surfaces)
            {
                if (surface.layerFormat != unknownFormat)
                    continue;
                if (surface.material.starts_with('*'))
                {
                    const auto layers = 1 + std::ranges::count(surface.material, '_');
                    const auto next = offsets.upper_bound(surface.layerOffset);
                    const auto gap = (next == offsets.end() ? static_cast<std::int64_t>(world.layerData.size()) : *next) - surface.layerOffset;
                    if ((layers == 3 || layers == 4) && surface.vertexCount && gap == 24 * surface.vertexCount)
                    {
                        surface.layerFormat = layers == 3 ? 5 : 6;
                        continue;
                    }
                }
                unknown.insert(surface.material);
            }
            if (!unknown.empty())
                warnings.push_back("world vertex format unknown for " + mod::ListRepr(std::vector<std::string>(unknown.begin(), unknown.end())) + "; layers dropped");
        }
    }

    GeometryMaterials PlanGeometryMaterials(world::RenderWorld& world, const std::vector<fs::path>& roots,
                                            extract::StockWaw* stock, Json& warnings)
    {
        ResolveFormats(world, roots, stock, warnings);
        std::set<std::string> names;
        for (const auto& surface : world.surfaces)
            names.insert(surface.material);
        GeometryMaterials plan;
        for (const auto& name : names)
        {
            const auto material = FindMaterial(roots, name);
            if (!material)
            {
                if (name.starts_with('*'))
                    plan.blendWeights.insert(name);
                continue;
            }
            const auto flags = material->value("gameFlags", Json::array());
            const auto flag = [&](const char* value) { return std::ranges::find(flags, value) != flags.end(); };
            if (flag("2") && (flag("CASTS_SHADOW") || flag("40")))
                plan.ownLightCasters.insert(name);
            const auto technique = TechniqueName(*material);
            if (technique == "wc_sky")
            {
                plan.sky.insert(name);
                if (!plan.skyImage)
                    for (const auto& tex : material->value("textures", Json::array()))
                        if (tex.value("name", std::string{}) == "colorMap" && mod::Truthy(tex, "image"))
                        {
                            plan.skyImage = tex.at("image").get<std::string>();
                            break;
                        }
            }
            else if (technique == "wc_tools")
                plan.tools.insert(name);
            else
            {
                const auto parsed = materials::ParseTechnique(technique);
                if (parsed.unlit == "shadowcaster" || flag("1"))
                    plan.shadowOnly.insert(name);
                else if (parsed.layers.size() > 1)
                    plan.blendWeights.insert(name);
            }
        }
        if (world.layerData.empty() && !plan.blendWeights.empty())
            warnings.push_back(std::to_string(plan.blendWeights.size()) + " layered materials drawn as their base layer; their vertex colour "
                               "(blend weights) is written as white (re-extract gfxworld with the v5 T4 dumper)");
        else
            plan.blendWeights.clear();
        return plan;
    }
}
