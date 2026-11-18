#include "models/surface_fixes.h"

#include <tomcrypt.h>

#include <cstring>
#include <limits>

namespace w2b::models
{
    namespace
    {
        struct AccessorView
        {
            std::size_t offset;
            std::size_t stride;
        };

        AccessorView View(const Json& gltf, const Json& accessor, std::size_t stride)
        {
            const auto& view = gltf.at("bufferViews").at(accessor.at("bufferView").get<std::size_t>());
            const auto start = view.value("byteOffset", std::size_t{0});
            const auto at = accessor.value("byteOffset", std::size_t{0});
            if (at > std::numeric_limits<std::size_t>::max() - start)
                throw std::runtime_error("glTF buffer offset overflows");
            return {start + at, view.value("byteStride", stride)};
        }

        std::size_t IndexSize(int type)
        {
            switch (type)
            {
            case 5121: return 1;
            case 5123: return 2;
            case 5125: return 4;
            default: throw std::runtime_error("invalid glTF index component type");
            }
        }

        std::string Decode(std::string_view payload)
        {
            auto length = static_cast<unsigned long>(payload.size() / 4 * 3 + 3);
            std::string output(length, '\0');
            if (base64_decode(payload.data(), static_cast<unsigned long>(payload.size()),
                              reinterpret_cast<unsigned char*>(output.data()), &length) != CRYPT_OK)
                throw std::runtime_error("invalid glTF buffer encoding");
            output.resize(length);
            return output;
        }

        std::string Encode(std::string_view data)
        {
            auto length = static_cast<unsigned long>(4 * ((data.size() + 2) / 3) + 1);
            std::string output(length, '\0');
            if (base64_encode(reinterpret_cast<const unsigned char*>(data.data()), static_cast<unsigned long>(data.size()),
                              output.data(), &length) != CRYPT_OK)
                throw std::runtime_error("failed to encode a staged glTF buffer");
            output.resize(length);
            return output;
        }

        std::size_t SetAlpha(std::string& bytes, const Json& gltf, const Json& primitive)
        {
            const auto& accessors = gltf.at("accessors");
            const auto& color = accessors.at(primitive.at("attributes").at("COLOR_0").get<std::size_t>());
            if (color.value("componentType", 0) != 5126 || color.value("type", std::string{}) != "VEC4")
                return 0;
            const auto colors = View(gltf, color, 16);
            const auto& index = accessors.at(primitive.at("indices").get<std::size_t>());
            const auto width = IndexSize(index.at("componentType").get<int>());
            const auto indices = View(gltf, index, width);
            const auto count = index.at("count").get<std::size_t>();
            if (indices.offset > bytes.size() || count > (bytes.size() - indices.offset) / width)
                throw std::runtime_error("glTF index accessor is outside its buffer");
            constexpr float opaque = 1.0f;
            std::size_t changed = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                std::uint32_t vertex = 0;
                std::memcpy(&vertex, bytes.data() + indices.offset + i * width, width);
                if (colors.offset > bytes.size() || bytes.size() - colors.offset < 16 || colors.stride == 0 ||
                    vertex > (bytes.size() - colors.offset - 16) / colors.stride)
                    throw std::runtime_error("glTF vertex color accessor is outside its buffer");
                auto* alpha = bytes.data() + colors.offset + vertex * colors.stride + 12;
                float previous = 0;
                std::memcpy(&previous, alpha, sizeof(previous));
                if (previous != opaque)
                {
                    std::memcpy(alpha, &opaque, sizeof(opaque));
                    ++changed;
                }
            }
            return changed;
        }
    }

    std::size_t OpaqueVertexAlpha(Json& gltf, const MaterialNames& materials)
    {
        if (materials.empty() || !gltf.contains("materials") || !gltf.contains("meshes"))
            return 0;
        std::vector<const Json*> targets;
        const auto& definitions = gltf.at("materials");
        for (const auto& mesh : gltf.at("meshes"))
        {
            const auto primitives = mesh.find("primitives");
            if (primitives == mesh.end())
                continue;
            for (const auto& primitive : *primitives)
            {
                const auto material = primitive.find("material");
                const auto index = primitive.find("indices");
                const auto attributes = primitive.find("attributes");
                if (material == primitive.end() || material->is_null() || index == primitive.end() || index->is_null() ||
                    attributes == primitive.end() || !attributes->contains("COLOR_0"))
                    continue;
                const auto& definition = definitions.at(material->get<std::size_t>());
                const auto name = definition.find("name");
                if (name != definition.end() && name->is_string() && materials.contains(name->get_ref<const std::string&>()))
                    targets.push_back(&primitive);
            }
        }
        if (targets.empty() || !gltf.contains("buffers") || gltf.at("buffers").empty())
            return 0;
        auto& buffer = gltf.at("buffers").at(0);
        auto uri = buffer.value("uri", std::string{});
        const auto split = uri.find(',');
        if (!uri.starts_with("data:") || split == uri.npos)
            return 0;
        auto bytes = Decode(std::string_view(uri).substr(split + 1));
        std::size_t changed = 0;
        for (const auto* primitive : targets)
            changed += SetAlpha(bytes, gltf, *primitive);
        if (changed != 0)
        {
            uri.resize(split + 1);
            buffer["uri"] = uri + Encode(bytes);
        }
        return changed;
    }
}
