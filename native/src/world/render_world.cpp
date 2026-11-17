#include "world/render_world.h"

#include "world/bsp_reader.h"
#include "world/layers.h"

#include <algorithm>
#include <span>
#include <string_view>

namespace w2b::world
{
    namespace
    {
        struct Header
        {
            std::uint32_t version;
            std::uint32_t vertices;
            std::uint32_t indices;
            std::uint32_t surfaces;
            std::uint32_t models;
        };

        Header ReadHeader(BspReader& input)
        {
            const auto magic = input.One<std::array<char, 8>>();
            static_assert(sizeof(Header) == 20);
            const auto header = input.One<Header>();
            if (std::string_view(magic.data(), magic.size()) != "W2BSP001" || header.version < 1 || header.version > 6)
                throw std::runtime_error("invalid W2BSP001 render-world header");
            if (header.vertices > 20'000'000 || header.indices > 60'000'000 || header.surfaces > 2'000'000)
                throw std::runtime_error("world counts exceed defensive limits");
            return header;
        }

        RenderGeometry ReadGeometry(BspReader& input, const Header& header, bool compensated)
        {
            return {ReadVertices(input, header.vertices, compensated), input.Array<std::uint16_t>(header.indices)};
        }

        WorldSurface ReadSurface(BspReader& input, int index, const RenderGeometry& geometry)
        {
            const auto first = input.One<std::uint32_t>();
            const auto count = input.One<std::uint16_t>();
            const auto triangles = input.One<std::uint16_t>();
            const auto base = input.One<std::uint32_t>();
            auto material = input.String();
            if (material.starts_with(','))
                material.erase(0, 1);
            const auto [lightmap, probe, primary, flags] = input.One<std::array<std::uint8_t, 4>>();
            const auto mins = input.One<std::array<float, 3>>();
            const auto maxs = input.One<std::array<float, 3>>();
            const auto indexCount = static_cast<std::size_t>(triangles) * 3;
            if (first + static_cast<std::size_t>(count) > geometry.vertices.size() ||
                base + indexCount > geometry.indices.size())
                throw std::runtime_error("render surface references data outside the world buffers");
            const auto indices = std::span(geometry.indices).subspan(base, indexCount);
            if (std::ranges::any_of(indices, [count](auto local) { return local >= count; }))
                throw std::runtime_error("render surface has an out-of-range local vertex index");
            return {first, count, triangles, base, std::move(material), lightmap, probe, primary, flags, mins, maxs,
                    0, 0, 0, -1, index, false};
        }

        WorldStaticModel ReadModel(BspReader& input, std::uint32_t version)
        {
            WorldStaticModel model{input.String(), input.One<std::array<float, 3>>(), input.One<std::array<float, 9>>(),
                                   input.One<float>(), input.One<std::uint32_t>()};
            if (version >= 2)
                model.cullDistance = input.One<float>();
            if (version >= 3)
            {
                const auto light = input.One<std::uint32_t>();
                if (light > 255)
                    throw std::runtime_error("static model primary-light index exceeds byte range");
                model.primaryLight = static_cast<std::uint8_t>(light);
            }
            return model;
        }

        void ReadBrushModels(BspReader& input, RenderWorld& world)
        {
            const auto count = input.One<std::uint32_t>();
            if (count < 1 || count > 65'536)
                throw std::runtime_error("unreasonable render brush model count");
            world.brushModelCount = count;
            input.CheckCount(count, 8);
            for (std::uint32_t owner = 0; owner < count; ++owner)
            {
                const auto [first, surfaces] = input.One<std::array<std::uint32_t, 2>>();
                if (surfaces && first + static_cast<std::size_t>(surfaces) > world.surfaces.size())
                    throw std::runtime_error("brush model surface range exceeds world surfaces");
                if (!owner || !surfaces)
                    continue;
                for (auto& surface : std::span(world.surfaces).subspan(first, surfaces))
                {
                    if (surface.brushModel)
                        throw std::runtime_error("overlapping brush model surface ranges");
                    surface.brushModel = owner;
                }
            }
        }

        void ReadLayers(BspReader& input, RenderWorld& world)
        {
            const auto size = input.One<std::uint32_t>();
            if (size > 256 * 1024 * 1024)
                throw std::runtime_error("unreasonable vertex layer buffer size");
            world.layerData = input.Array<std::uint8_t>(size);
            for (auto& surface : world.surfaces)
            {
                surface.layerOffset = input.One<std::int32_t>();
                surface.layerFormat = input.One<std::uint8_t>();
                const auto stride = surface.layerFormat == 0xff ? 0 : LayerFormat(surface.layerFormat).Stride();
                if (stride && (surface.layerOffset < 0 ||
                    static_cast<std::uint64_t>(surface.layerOffset) + stride * surface.vertexCount > size))
                    throw std::runtime_error("surface layer data exceeds the layer buffer (" + surface.material + ')');
            }
        }

        ShadowLight ReadShadowLight(BspReader& input, const RenderWorld& world)
        {
            const auto [surfaceCount, modelCount] = input.One<std::array<std::uint16_t, 2>>();
            ShadowLight light{input.Array<std::uint16_t>(surfaceCount), input.Array<std::uint16_t>(modelCount), {}};
            if (std::ranges::any_of(light.surfaces, [&](auto id) { return id >= world.surfaces.size(); }) ||
                std::ranges::any_of(light.staticModels, [&](auto id) { return id >= world.staticModels.size(); }))
                throw std::runtime_error("shadow geometry references data outside the world");
            const auto hulls = input.One<std::uint32_t>();
            input.CheckCount(hulls, 76);
            light.hulls.reserve(hulls);
            for (std::uint32_t hull = 0; hull < hulls; ++hull)
                light.hulls.push_back({input.One<std::array<float, 9>>(), input.One<std::array<float, 9>>(),
                                       input.Array<std::array<float, 5>>(input.One<std::uint32_t>())});
            return light;
        }
    }

    RenderWorld ReadRenderWorld(const std::filesystem::path& path, bool compensated)
    {
        BspReader input(path);
        const auto header = ReadHeader(input);
        RenderWorld world;
        world.name = input.String();
        world.baseName = input.String();
        world.skyboxModel = input.String();
        world.geometry = ReadGeometry(input, header, compensated);
        input.CheckCount(header.surfaces, 44);
        world.surfaces.reserve(header.surfaces);
        for (std::uint32_t i = 0; i < header.surfaces; ++i)
            world.surfaces.push_back(ReadSurface(input, static_cast<int>(i), world.geometry));
        input.CheckCount(header.models, 60 + 4 * (std::min(header.version, 3u) - 1));
        world.staticModels.reserve(header.models);
        for (std::uint32_t i = 0; i < header.models; ++i)
            world.staticModels.push_back(ReadModel(input, header.version));
        if (header.version >= 4)
            ReadBrushModels(input, world);
        if (header.version >= 5)
            ReadLayers(input, world);
        if (header.version >= 6)
        {
            const auto lights = input.One<std::uint32_t>();
            input.CheckCount(lights, 8);
            world.shadowLights.reserve(lights);
            for (std::uint32_t i = 0; i < lights; ++i)
                world.shadowLights.push_back(ReadShadowLight(input, world));
        }
        if (input.Remaining())
            throw std::runtime_error("render-world file has trailing data (schema mismatch)");
        return world;
    }
}
