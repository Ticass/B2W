#include "LoaderImageT6.h"

#include "Game/T6/CommonT6.h"
#include "Game/T6/T6.h"
#include "Image/ImageCommon.h"
#include "Image/IwiLoader.h"
#include "Image/IwiTypes.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <zlib.h>

using namespace T6;

namespace
{
    class ImageLoader final : public AssetCreator<AssetImage>
    {
    public:
        ImageLoader(MemoryManager& memory, ISearchPath& searchPath)
            : m_memory(memory),
              m_search_path(searchPath)
        {
        }

        // images/streaming.json {"streamingMode": {"<image>": 2}} from the
        // bridge stage. Stock effect images (all 125 in zm_nuked/common_zm) are
        // streaming 2 with a header-only loadDef; under the world streaming mode
        // (1) effect images are never made resident and sprites sample a
        // fallback.
        int StreamingMode(const std::string& assetName)
        {
            if (!m_streaming_loaded)
            {
                m_streaming_loaded = true;
                const auto file = m_search_path.Open("images/streaming.json");
                if (file.IsOpen())
                {
                    const auto json = nlohmann::json::parse(*file.m_stream);
                    for (const auto& [name, mode] : json.at("streamingMode").items())
                        m_streaming[name] = mode.get<int>();
                }
            }
            const auto entry = m_streaming.find(assetName);
            return entry != m_streaming.end() ? entry->second : 1;
        }

        AssetCreationResult CreateAsset(const std::string& assetName, AssetCreationContext& context) override
        {
            const auto fileName = image::GetFileNameForAsset(assetName, ".iwi");
            const auto file = m_search_path.Open(fileName);
            if (!file.IsOpen())
                return AssetCreationResult::NoAction();

            const auto fileSize = static_cast<size_t>(file.m_length);
            const auto fileData = std::make_unique<char[]>(fileSize);
            file.m_stream->read(fileData.get(), static_cast<std::streamsize>(fileSize));
            const auto dataHash = static_cast<unsigned>(crc32(0u, reinterpret_cast<const Bytef*>(fileData.get()), static_cast<unsigned>(fileSize)));

            std::istringstream ss(std::string(fileData.get(), fileSize));
            const auto texture = iwi::LoadIwi(ss);
            if (!texture)
            {
                con::error("Failed to load texture from: {}", fileName);
                return AssetCreationResult::Failure();
            }

            auto* image = m_memory.Alloc<GfxImage>();
            image->name = m_memory.Dup(assetName.c_str());
            image->hash = Common::R_HashString(image->name, 0);
            image->delayLoadPixels = true;

            image->noPicmip = !texture->HasMipMaps();
            image->width = static_cast<uint16_t>(texture->GetWidth());
            image->height = static_cast<uint16_t>(texture->GetHeight());
            image->depth = static_cast<uint16_t>(texture->GetDepth());

            // Runtime metadata, matching stock streamed images (zm_nuked):
            // mapType 2D=3 / 3D=4 / CUBE=5, category LOAD_FROM_FILE (LIGHTMAP
            // for lightmaps), card memory = mip data size, real mip count.
            // Left at 0 the renderer binds images with no type (colour casts,
            // black cube maps). The semantic is set by the material that uses it.
            switch (texture->GetTextureType())
            {
            case TextureType::T_CUBE:
                image->mapType = 5;
                break;
            case TextureType::T_3D:
                image->mapType = 4;
                break;
            default:
                image->mapType = 3;
                break;
            }
            image->category = assetName.find("lightmap") != std::string::npos ? IMG_CATEGORY_LIGHTMAP : IMG_CATEGORY_LOAD_FROM_FILE;
            constexpr size_t IWI_HEADER_SIZE = 64;
            image->cardMemory.platform[0] = static_cast<int>(fileSize > IWI_HEADER_SIZE ? fileSize - IWI_HEADER_SIZE : 0);
            image->cardMemory.platform[1] = image->cardMemory.platform[0];

            // Stock world lightmaps and reflection probes are not streamed: the
            // pixels live inline in the zone (loadDef, streaming 0). Streamed
            // copies are never made resident for the world shaders, which then
            // sample a fallback texture (red/pink cast on every world surface).
            // Only the lightmap: an inline 64x64 mipped cube probe / R8 $outdoor was
            // rejected by the game with E_INVALIDARG while loading the zone.
            const bool inlineImage = assetName.find("lightmap") != std::string::npos;
            if (inlineImage)
            {
                size_t dataSize = 0;
                const auto mipCount = texture->HasMipMaps() ? texture->GetMipMapCount() : 1;
                for (auto mip = 0; mip < mipCount; mip++)
                    dataSize += texture->GetSizeOfMipLevel(mip) * texture->GetFaceCount();

                auto* loadDef = static_cast<GfxImageLoadDef*>(m_memory.AllocRaw(offsetof(GfxImageLoadDef, data) + dataSize));
                loadDef->levelCount = static_cast<char>(mipCount);
                loadDef->flags = 0;
                if (!texture->HasMipMaps())
                    loadDef->flags |= iwi27::IMG_FLAG_NOMIPMAPS;
                if (texture->GetTextureType() == TextureType::T_CUBE)
                    loadDef->flags |= iwi27::IMG_FLAG_CUBEMAP;
                else if (texture->GetTextureType() == TextureType::T_3D)
                    loadDef->flags |= iwi27::IMG_FLAG_VOLMAP;
                loadDef->format = static_cast<int>(texture->GetFormat()->GetDxgiFormat());
                loadDef->resourceSize = static_cast<int>(dataSize);
                std::memcpy(loadDef->data, texture->GetBufferForMipLevel(0), dataSize);

                image->texture.loadDef = loadDef;
                image->delayLoadPixels = false;
                image->levelCount = 0; // as stock inline lightmaps
                image->streaming = 0;
                image->baseSize = static_cast<unsigned>(dataSize);
                image->cardMemory.platform[0] = static_cast<int>(dataSize);
                image->cardMemory.platform[1] = static_cast<int>(dataSize);
                image->streamedPartCount = 0;
                // stock: semantic 1 for all three, category LIGHTMAP (2) for lightmaps, 1 for probe/$outdoor
                image->semantic = 1;
                if (image->category != IMG_CATEGORY_LIGHTMAP)
                    image->category = 1;
                con::info("Image {}: inline, {}x{}, dxgi {}, {} bytes", assetName, image->width, image->height, loadDef->format, dataSize);
                return AssetCreationResult::Success(context.AddAsset<AssetImage>(assetName, image));
            }

            image->streaming = 1;
            if (StreamingMode(assetName) == 2)
            {
                // stock streaming-2 header: no pixels inline (resourceSize 0),
                // levelCount 0, or 1 with NOMIPMAPS for unmipped images
                auto* loadDef = m_memory.Alloc<GfxImageLoadDef>();
                loadDef->levelCount = texture->HasMipMaps() ? 0 : 1;
                loadDef->flags = 0;
                if (!texture->HasMipMaps())
                    loadDef->flags |= iwi27::IMG_FLAG_NOMIPMAPS;
                if (texture->GetTextureType() == TextureType::T_CUBE)
                    loadDef->flags |= iwi27::IMG_FLAG_CUBEMAP;
                else if (texture->GetTextureType() == TextureType::T_3D)
                    loadDef->flags |= iwi27::IMG_FLAG_VOLMAP;
                loadDef->format = static_cast<int>(texture->GetFormat()->GetDxgiFormat());
                loadDef->resourceSize = 0;
                image->texture.loadDef = loadDef;
                image->streaming = 2;
            }
            image->streamedParts[0].levelCount = static_cast<uint32_t>(std::min(texture->GetMipMapCount(), 15));
            image->streamedParts[0].levelSize = static_cast<uint32_t>(fileSize);
            image->streamedParts[0].hash = dataHash & 0x1FFFFFFF;
            image->streamedPartCount = 1;

            return AssetCreationResult::Success(context.AddAsset<AssetImage>(assetName, image));
        }

    private:
        MemoryManager& m_memory;
        ISearchPath& m_search_path;
        bool m_streaming_loaded = false;
        std::unordered_map<std::string, int> m_streaming;
    };
} // namespace

namespace image
{
    std::unique_ptr<AssetCreator<AssetImage>> CreateLoaderT6(MemoryManager& memory, ISearchPath& searchPath)
    {
        return std::make_unique<ImageLoader>(memory, searchPath);
    }
} // namespace image
