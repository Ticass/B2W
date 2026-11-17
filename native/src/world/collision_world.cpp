#include "world/collision_world.h"

#include "world/bsp_reader.h"

#include <algorithm>
#include <string_view>

namespace w2b::world
{
    namespace
    {
        static_assert(sizeof(BrushPlane) == 20 && sizeof(ClipPoint) == 12);
        static_assert(sizeof(TerrainNode) == 8 && sizeof(TerrainRoot) == 12 && sizeof(TerrainPartition) == 8);

        template <typename T, typename Read>
        std::vector<T> ReadRecords(BspReader& input, std::size_t minimumSize, Read read)
        {
            const auto count = input.One<std::uint32_t>();
            input.CheckCount(count, minimumSize);
            std::vector<T> records;
            records.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index)
                records.push_back(read(input));
            return records;
        }

        template <typename T>
        std::vector<T> CountedArray(BspReader& input)
        {
            return input.Array<T>(input.One<std::uint32_t>());
        }

        ClipMaterial ReadMaterial(BspReader& input)
        {
            return {input.String(), input.One<std::uint32_t>(), input.One<std::uint32_t>()};
        }

        ClipBrush ReadBrush(BspReader& input)
        {
            return {input.One<ClipPoint>(), input.One<ClipPoint>(), input.One<std::uint32_t>(),
                    CountedArray<BrushPlane>(input), CountedArray<ClipPoint>(input)};
        }

        ClipModel ReadClipModel(BspReader& input, std::size_t brushCount)
        {
            ClipModel model{input.One<ClipPoint>(), input.One<ClipPoint>(), CountedArray<std::uint16_t>(input)};
            if (std::ranges::any_of(model.brushes, [brushCount](auto brush) { return brush >= brushCount; }))
                throw std::runtime_error("submodel references a brush outside the brush array");
            return model;
        }

        StaticCollisionSurface ReadStaticSurface(BspReader& input)
        {
            return {input.One<std::uint32_t>(), input.One<std::uint32_t>(), input.One<ClipPoint>(),
                    input.One<ClipPoint>(), input.One<std::int32_t>(), CountedArray<std::array<float, 12>>(input)};
        }

        ClipStaticModel ReadStaticModel(BspReader& input)
        {
            return {input.String(), input.One<ClipPoint>(), input.One<std::array<float, 9>>(),
                    input.One<ClipPoint>(), input.One<ClipPoint>(), input.One<std::uint32_t>(),
                    ReadRecords<StaticCollisionSurface>(input, 40, ReadStaticSurface)};
        }
    }

    CollisionWorld ReadCollisionWorld(const std::filesystem::path& path)
    {
        BspReader input(path);
        const auto magic = input.One<std::array<char, 8>>();
        const auto version = input.One<std::uint32_t>();
        if (std::string_view(magic.data(), magic.size()) != "W2BSP001" || version < 2 || version > 9)
            throw std::runtime_error("invalid W2BSP001 clip-world header");

        CollisionWorld world;
        world.name = input.String();
        world.materials = ReadRecords<ClipMaterial>(input, 12, ReadMaterial);
        world.vertices = CountedArray<ClipPoint>(input);
        const std::size_t triangleCount = input.One<std::uint32_t>();
        world.indices = input.Array<std::uint16_t>(triangleCount * 3);
        world.brushes = ReadRecords<ClipBrush>(input, 36, ReadBrush);
        if (version >= 3)
        {
            world.models = ReadRecords<ClipModel>(input, 28, [&](BspReader& reader)
            {
                return ReadClipModel(reader, world.brushes.size());
            });
            world.staticModels = ReadRecords<ClipStaticModel>(input, 84, ReadStaticModel);
        }
        if (version >= 4)
        {
            const auto count = input.One<std::uint32_t>();
            if (count != triangleCount)
                throw std::runtime_error("triangle material table does not match the triangle count");
            world.triangleMaterials = input.Array<std::uint16_t>(count);
            if (std::ranges::any_of(world.triangleMaterials, [&](auto material)
                { return material != 0xffff && material >= world.materials.size(); }))
                throw std::runtime_error("collision triangle references a material outside the material table");
        }
        world.brushOwnershipComplete = version >= 5;
        if (version >= 6)
        {
            const auto size = input.One<std::uint32_t>();
            if (size != (3 * triangleCount + 31) / 32 * 4)
                throw std::runtime_error("walkable edge table does not match the triangle count");
            world.edgeWalkable = input.Array<std::uint8_t>(size);
        }
        if (version >= 7)
        {
            world.terrainNodes = CountedArray<TerrainNode>(input);
            world.leafRoots = CountedArray<TerrainRoot>(input);
            world.partitions = CountedArray<TerrainPartition>(input);
        }
        if (version >= 8)
        {
            const auto count = input.One<std::uint32_t>();
            if (count != world.brushes.size())
                throw std::runtime_error("brush reach table does not match the brush count");
            world.brushReach = input.Array<std::uint32_t>(count);
        }
        if (version >= 9)
        {
            const auto count = input.One<std::uint32_t>();
            if (count != world.models.size())
                throw std::runtime_error("terrain model roots do not match the submodel count");
            world.modelRoots = input.Array<TerrainRoot>(count);
        }
        if (input.Remaining())
            throw std::runtime_error("clip-world file has trailing data (schema mismatch)");
        return world;
    }
}
