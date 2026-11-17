#include "weapons/skeleton.h"

#include "weapons/weapon.h"

#include <tomcrypt.h>

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

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
