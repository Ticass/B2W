#include "ComWorldLinker.h"
#include "Game/T6/BSP/BSPUtil.h"
#include <nlohmann/json.hpp>
#include <limits>
#include <format>

using namespace T6;
using nlohmann::json;

namespace
{
    class LightDefLoader final : public AssetCreator<AssetLightDef>
    {
        MemoryManager& memory;
        ISearchPath& searchPath;
    public:
        LightDefLoader(MemoryManager& m, ISearchPath& s) : memory(m), searchPath(s) {}
        AssetCreationResult CreateAsset(const std::string& name, AssetCreationContext& context) override
        {
            const auto file = searchPath.Open(std::format("lightdef/{}.json", name));
            if (!file.IsOpen()) return AssetCreationResult::NoAction();
            try
            {
                const auto js = json::parse(*file.m_stream);
                auto* def = memory.Alloc<GfxLightDef>();
                *def = {};
                def->name = memory.Dup(name.c_str());
                def->attenuation.samplerState = static_cast<char>(js.at("samplerState").get<unsigned char>());
                def->lmapLookupStart = js.at("lmapLookupStart").get<int>();
                AssetRegistration<AssetLightDef> registration(name, def);
                const auto imageName = js.at("attenuation").get<std::string>();
                if (!imageName.empty())
                {
                    auto* image = context.LoadDependency<AssetImage>(imageName);
                    if (!image) return AssetCreationResult::Failure();
                    def->attenuation.image = image->Asset();
                    registration.AddDependency(image);
                }
                return AssetCreationResult::Success(context.AddAsset(std::move(registration)));
            }
            catch (const json::exception& e)
            {
                con::error("Invalid light definition {}: {}", name, e.what());
                return AssetCreationResult::Failure();
            }
        }
    };
}

namespace BSP
{
    std::unique_ptr<AssetCreator<AssetLightDef>> CreateLightDefLoader(MemoryManager& memory, ISearchPath& searchPath)
    {
        return std::make_unique<LightDefLoader>(memory, searchPath);
    }
    ComWorldLinker::ComWorldLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context)
        : m_memory(memory),
          m_search_path(searchPath),
          m_context(context)
    {
    }

    ComWorld* ComWorldLinker::LinkComWorld(const BSPData& bsp) const
    {
        // all lights that aren't the sunlight or default light need their own GfxLightDef asset
        ComWorld* comWorld = m_memory.Alloc<ComWorld>();

        comWorld->name = m_memory.Dup(bsp.bspName.c_str());
        comWorld->isInUse = 1;
        const auto sourceFile = m_search_path.Open(GetFileNameForBSPAsset("primarylights.json"));
        if (sourceFile.IsOpen())
        {
            try
            {
                const auto js = json::parse(*sourceFile.m_stream);
                const auto& lights = js.at("lights");
                if (lights.size() < 2 || lights.size() > 255)
                    throw std::runtime_error("primary light count must be in 2..255");
                comWorld->primaryLightCount = static_cast<unsigned>(lights.size());
                comWorld->primaryLights = m_memory.Alloc<ComPrimaryLight>(lights.size());
                for (size_t i = 0; i < lights.size(); ++i)
                {
                    const auto& row = lights[i];
                    auto& light = comWorld->primaryLights[i];
                    light = {};
                    light.type = row.at("type").get<char>();
                    light.canUseShadowMap = row.at("canUseShadowMap").get<char>();
                    light.exponent = row.at("exponent").get<char>();
                    light.priority = row.at("priority").get<char>();
                    light.cullDist = row.at("cullDist").get<int16_t>();
                    const auto readVec = [](const json& v) { return vec3_t{{.x=v.at(0).get<float>(), .y=v.at(1).get<float>(), .z=v.at(2).get<float>()}}; };
                    light.color = readVec(row.at("color"));
                    light.diffuseColor.r = light.color.x;
                    light.diffuseColor.g = light.color.y;
                    light.diffuseColor.b = light.color.z;
                    light.diffuseColor.a = 1.0f;
                    light.dir = readVec(row.at("dir"));
                    light.origin = readVec(row.at("origin"));
#define READ_LIGHT_FLOAT(field) light.field = row.at(#field).get<float>()
                    READ_LIGHT_FLOAT(radius); READ_LIGHT_FLOAT(cosHalfFovOuter); READ_LIGHT_FLOAT(cosHalfFovInner);
                    READ_LIGHT_FLOAT(cosHalfFovExpanded); READ_LIGHT_FLOAT(rotationLimit); READ_LIGHT_FLOAT(translationLimit);
#undef READ_LIGHT_FLOAT
                    // T6 sub_782FA0 builds lightFallOffA/B and lightSpotDir.w
                    // from these precomputed fields (waw2bo2 lighting.t6_light_fields)
                    const auto readVec4 = [](const json& v) { return vec4_t{{v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>(), v.at(3).get<float>()}}; };
                    if (row.contains("falloff"))
                        light.falloff = readVec4(row.at("falloff"));
                    if (row.contains("aAbB"))
                        light.aAbB = readVec4(row.at("aAbB"));
                    if (row.contains("dAttenuation"))
                        light.dAttenuation = row.at("dAttenuation").get<float>();
                    // sub_73AC60 makes roundness 0 spots SPOT_SQUARE, 1 SPOT_ROUND
                    if (row.contains("roundness"))
                        light.roundness = row.at("roundness").get<float>();
                    const auto name = row.at("defName").get<std::string>();
                    if (!name.empty())
                    {
                        if (!m_context.LoadDependency<AssetLightDef>(name)) return nullptr;
                        light.defName = m_memory.Dup(name.c_str());
                    }
                }
                con::info("Loaded {} source primary lights", lights.size());
                return comWorld;
            }
            catch (const std::exception& e)
            {
                con::error("Invalid primary lights: {}", e.what());
                return nullptr;
            }
        }
        comWorld->primaryLightCount = BSPGameConstants::BSP_DEFAULT_LIGHT_COUNT;
        comWorld->primaryLights = m_memory.Alloc<ComPrimaryLight>(comWorld->primaryLightCount);

        // first (static) light is always empty

        ComPrimaryLight* sunLight = &comWorld->primaryLights[1];
        const vec4_t sunLightColor = BSPEditableConstants::SUNLIGHT_COLOR;
        const vec3_t sunLightDirection = BSPEditableConstants::SUNLIGHT_DIRECTION;

        sunLight->type = GFX_LIGHT_TYPE_DIR;
        sunLight->diffuseColor.r = sunLightColor.r;
        sunLight->diffuseColor.g = sunLightColor.g;
        sunLight->diffuseColor.b = sunLightColor.b;
        sunLight->diffuseColor.a = sunLightColor.a;
        sunLight->dir.x = sunLightDirection.x;
        sunLight->dir.y = sunLightDirection.y;
        sunLight->dir.z = sunLightDirection.z;

        return comWorld;
    }
} // namespace BSP
