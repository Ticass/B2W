#include "weapons/skeleton.h"

#include "weapons/weapon.h"

#include <tomcrypt.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <type_traits>
#include <vector>

namespace w2b::weapons
{
    namespace
    {
        std::optional<std::size_t> RootIndex(const Json& gltf, const Json& joints)
        {
            std::set<std::size_t> children;
            for (const auto& joint : joints)
            {
                const auto& node = gltf.at("nodes").at(joint.get<std::size_t>());
                if (const auto list = node.find("children"); list != node.end())
                    for (const auto& child : *list)
                        children.insert(child.get<std::size_t>());
            }
            std::optional<std::size_t> root;
            for (std::size_t i = 0; i < joints.size(); ++i)
            {
                if (children.contains(joints.at(i).get<std::size_t>()))
                    continue;
                if (root)
                    return std::nullopt;
                root = i;
            }
            return root;
        }

        std::size_t AccessorOffset(const Json& gltf, const Json& accessor)
        {
            const auto& view = gltf.at("bufferViews").at(accessor.at("bufferView").get<std::size_t>());
            const auto base = view.value("byteOffset", std::size_t{});
            const auto offset = accessor.value("byteOffset", std::size_t{});
            if (offset > std::numeric_limits<std::size_t>::max() - base)
                throw WeaponError("GLTF skin accessor lies outside its buffer");
            return base + offset;
        }

        struct BindRange
        {
            std::size_t jointOffset, weightOffset, count;
        };

        std::optional<std::vector<BindRange>> FindRanges(const Json& gltf)
        {
            std::set<std::pair<std::size_t, std::size_t>> accessors;
            for (const auto& mesh : gltf.at("meshes"))
                for (const auto& primitive : mesh.at("primitives"))
                {
                    const auto& attributes = primitive.at("attributes");
                    const auto joints = attributes.find("JOINTS_0");
                    const auto weights = attributes.find("WEIGHTS_0");
                    if (joints == attributes.end() || joints->is_null() || weights == attributes.end() || weights->is_null())
                        return std::nullopt;
                    accessors.emplace(joints->get<std::size_t>(), weights->get<std::size_t>());
                }
            std::vector<BindRange> ranges;
            for (const auto& [jointIndex, weightIndex] : accessors)
            {
                const auto& joint = gltf.at("accessors").at(jointIndex);
                const auto& weight = gltf.at("accessors").at(weightIndex);
                if (joint.at("componentType") != 5121 || weight.at("componentType") != 5126 ||
                    joint.at("type") != "VEC4" || weight.at("type") != "VEC4")
                    return std::nullopt;
                ranges.push_back({AccessorOffset(gltf, joint), AccessorOffset(gltf, weight), joint.at("count").get<std::size_t>()});
            }
            return ranges;
        }

        template<std::size_t Width>
        void Repeat(std::string& buffer, std::size_t offset, std::size_t count, const std::array<unsigned char, Width>& value)
        {
            if (offset > buffer.size() || count > (buffer.size() - offset) / Width)
                throw WeaponError("GLTF skin accessor lies outside its buffer");
            for (auto* dest = buffer.data() + offset; count != 0; --count, dest += Width)
                std::memcpy(dest, value.data(), Width);
        }
    }

    namespace
    {
        std::optional<std::string> DecodeBuffer(const Json& gltf)
        {
            const auto buffers = gltf.find("buffers");
            if (buffers == gltf.end() || buffers->size() != 1)
                return std::nullopt;
            const auto& uri = buffers->at(0).at("uri").get_ref<const std::string&>();
            static constexpr std::string_view separator = ";base64,";
            const auto split = uri.find(separator);
            if (split == uri.npos)
                return std::nullopt;
            const auto payload = std::string_view(uri).substr(split + separator.size());
            auto size = static_cast<unsigned long>(payload.size() / 4 * 3);
            std::string buffer(size, '\0');
            if (base64_strict_decode(payload.data(), static_cast<unsigned long>(payload.size()),
                                    reinterpret_cast<unsigned char*>(buffer.data()), &size) != CRYPT_OK)
                return std::nullopt;
            buffer.resize(size);
            return buffer;
        }

        // the accessor's elements as unsigned integers (indices, joints) or floats (weights)
        template<typename T>
        std::optional<std::vector<T>> ReadAccessor(const Json& gltf, const std::string& buffer, std::size_t index, std::size_t width)
        {
            const auto& accessor = gltf.at("accessors").at(index);
            const auto type = accessor.at("componentType").get<int>();
            const std::size_t size = type == 5121 ? 1 : type == 5123 ? 2 : type == 5125 || type == 5126 ? 4 : 0;
            if (size == 0 || (std::is_floating_point_v<T> != (type == 5126)))
                return std::nullopt;
            const auto& view = gltf.at("bufferViews").at(accessor.at("bufferView").get<std::size_t>());
            const auto stride = view.value("byteStride", size * width);
            const auto offset = AccessorOffset(gltf, accessor);
            const auto count = accessor.at("count").get<std::size_t>();
            if (count != 0 && (offset > buffer.size() || (count - 1) * stride + size * width > buffer.size() - offset))
                return std::nullopt;
            std::vector<T> out;
            out.reserve(count * width);
            for (std::size_t i = 0; i < count; ++i)
                for (std::size_t k = 0; k < width; ++k)
                {
                    const auto* at = buffer.data() + offset + i * stride + k * size;
                    if constexpr (std::is_floating_point_v<T>)
                    {
                        float value;
                        std::memcpy(&value, at, 4);
                        out.push_back(value);
                    }
                    else
                    {
                        std::uint32_t value = 0;
                        std::memcpy(&value, at, size);
                        out.push_back(static_cast<T>(value));
                    }
                }
            return out;
        }

        struct Surface
        {
            std::size_t vertices = 0;
            bool skinned = false;
        };

        // what the mod tools linker (Linker.exe sub_50AA30) sees of one mesh: the vertices its triangles
        // use, and whether it is skinned (sub_50C0E0: a vertex with several weights, or a triangle whose
        // vertices follow different bones)
        std::optional<Surface> MeasureMesh(const Json& gltf, const std::string& buffer, const Json& mesh)
        {
            Surface surface;
            for (const auto& primitive : mesh.at("primitives"))
            {
                const auto& attributes = primitive.at("attributes");
                if (!primitive.contains("indices") || !attributes.contains("JOINTS_0") || !attributes.contains("WEIGHTS_0"))
                    return std::nullopt;
                const auto indices = ReadAccessor<std::uint32_t>(gltf, buffer, primitive.at("indices").get<std::size_t>(), 1);
                const auto joints = ReadAccessor<std::uint32_t>(gltf, buffer, attributes.at("JOINTS_0").get<std::size_t>(), 4);
                const auto weights = ReadAccessor<float>(gltf, buffer, attributes.at("WEIGHTS_0").get<std::size_t>(), 4);
                if (!indices || !joints || !weights || indices->size() % 3 != 0)
                    return std::nullopt;
                const auto vertexCount = joints->size() / 4;
                std::set<std::uint32_t> used;
                std::optional<std::uint32_t> bones[3];
                for (std::size_t i = 0; i < indices->size(); ++i)
                {
                    const auto v = (*indices)[i];
                    if (v >= vertexCount)
                        return std::nullopt;
                    used.insert(v);
                    int influences = 0;
                    std::uint32_t bone = 0;
                    for (std::size_t k = 0; k < 4; ++k)
                        if ((*weights)[v * 4 + k] > 0.0f)
                        {
                            ++influences;
                            bone = (*joints)[v * 4 + k];
                        }
                    if (influences > 1)
                        surface.skinned = true;
                    bones[i % 3] = bone;
                    if (i % 3 == 2 && !(*bones[0] == *bones[1] && *bones[1] == *bones[2]))
                        surface.skinned = true;
                }
                surface.vertices += used.size();
            }
            return surface;
        }
    }

    bool OrderSurfacesForLinker(Json& gltf, std::size_t animatedLimit, std::size_t rigidLimit)
    {
        const auto buffer = DecodeBuffer(gltf);
        const auto nodes = gltf.find("nodes");
        const auto meshes = gltf.find("meshes");
        if (!buffer || nodes == gltf.end() || meshes == gltf.end())
            return false;
        // the linker takes a node's meshes breadth first: the mesh nodes must be siblings to be reordered
        std::optional<std::size_t> parent;
        std::size_t meshNodes = 0;
        for (std::size_t i = 0; i < nodes->size(); ++i)
            if (nodes->at(i).contains("mesh"))
                ++meshNodes;
        for (std::size_t i = 0; i < nodes->size() && !parent; ++i)
        {
            const auto children = nodes->at(i).find("children");
            if (children == nodes->at(i).end())
                continue;
            std::size_t found = 0;
            for (const auto& child : *children)
                if (nodes->at(child.get<std::size_t>()).contains("mesh"))
                    ++found;
            if (found == meshNodes && found != 0)
                parent = i;
        }
        if (!parent)
            return false;
        auto& children = nodes->at(*parent).at("children");
        std::vector<std::pair<Json, Surface>> skinned, rigid, others;
        for (const auto& child : children)
        {
            const auto& node = nodes->at(child.get<std::size_t>());
            if (!node.contains("mesh"))
            {
                others.push_back({child, {}});
                continue;
            }
            const auto surface = MeasureMesh(gltf, *buffer, meshes->at(node.at("mesh").get<std::size_t>()));
            if (!surface)
                return false;
            (surface->skinned ? skinned : rigid).push_back({child, *surface});
        }
        // Linker.exe stores each surface's first vertex in 16 bits: a skinned surface must end within
        // animatedLimit, a rigid one within rigidLimit, counted over the LOD's surfaces in order
        std::size_t base = 0;
        Json ordered = Json::array();
        for (const auto* group : {&skinned, &rigid})
            for (const auto& [child, surface] : *group)
            {
                base += surface.vertices;
                if (base > (group == &skinned ? animatedLimit : rigidLimit))
                    return false;
                ordered.push_back(child);
            }
        for (const auto& [child, surface] : others)
            ordered.push_back(child);
        children = std::move(ordered);
        return true;
    }

    bool RebindToRoot(Json& gltf)
    {
        const auto skins = gltf.find("skins");
        const auto buffers = gltf.find("buffers");
        if (skins == gltf.end() || skins->size() != 1 || buffers == gltf.end() || buffers->size() != 1)
            return false;
        auto& uri = buffers->at(0).at("uri").get_ref<std::string&>();
        static constexpr std::string_view separator = ";base64,";
        const auto split = uri.find(separator);
        const auto root = RootIndex(gltf, skins->at(0).at("joints"));
        if (split == uri.npos || !root)
            return false;
        const auto ranges = FindRanges(gltf);
        if (!ranges)
            return false;
        if (*root > 255)
            throw WeaponError("bytes must be in range(0, 256)");
        const auto payload = std::string_view(uri).substr(split + separator.size());
        if (payload.size() > std::numeric_limits<unsigned long>::max())
            throw WeaponError("GLTF skin buffer is too large");
        auto size = static_cast<unsigned long>(payload.size() / 4 * 3);
        std::string buffer(size, '\0');
        if (base64_strict_decode(payload.data(), static_cast<unsigned long>(payload.size()),
                                reinterpret_cast<unsigned char*>(buffer.data()), &size) != CRYPT_OK)
            throw WeaponError("Incorrect padding");
        buffer.resize(size);
        const std::array<unsigned char, 4> joints{static_cast<unsigned char>(*root), 0, 0, 0};
        // Little-endian IEEE 754 encoding of four weights: (1.0, 0.0, 0.0, 0.0).
        static constexpr std::array<unsigned char, 16> weights{0, 0, 0x80, 0x3f};
        for (const auto& range : *ranges)
        {
            Repeat(buffer, range.jointOffset, range.count, joints);
            Repeat(buffer, range.weightOffset, range.count, weights);
        }
        auto encodedSize = static_cast<unsigned long>(4 * ((buffer.size() + 2) / 3) + 1);
        const auto prefix = split + separator.size();
        uri.resize(prefix + encodedSize);
        if (base64_encode(reinterpret_cast<const unsigned char*>(buffer.data()), size, uri.data() + prefix, &encodedSize) != CRYPT_OK)
            throw WeaponError("could not encode GLTF skin buffer");
        uri.resize(prefix + encodedSize);
        return true;
    }
}
