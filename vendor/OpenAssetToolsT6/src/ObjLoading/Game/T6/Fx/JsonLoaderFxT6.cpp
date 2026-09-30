#include "JsonLoaderFxT6.h"

#include "Game/T6/T6.h"
#include "Utils/Logging/Log.h"

#include <bit>
#include <format>
#include <nlohmann/json.hpp>

using namespace nlohmann;
using namespace T6;

namespace
{
    class FxLoadError final : public std::runtime_error
    {
    public:
        explicit FxLoadError(const std::string& msg)
            : std::runtime_error(msg)
        {
        }
    };

    class FxJsonReader
    {
    public:
        FxJsonReader(MemoryManager& memory, AssetCreationContext& context, AssetRegistration<AssetFx>& registration)
            : m_memory(memory),
              m_context(context),
              m_registration(registration)
        {
        }

        void Read(const json& j, FxEffectDef& fx)
        {
            if (j.value("_type", "") != "waw2bo2_fx" || j.value("_game", "") != "T6")
                throw FxLoadError("not a T6 waw2bo2_fx file");

            fx.flags = j.at("flags").get<uint16_t>();
            fx.efPriority = static_cast<char>(j.at("efPriority").get<int>());
            fx.elemDefCountLooping = j.at("elemDefCountLooping").get<int16_t>();
            fx.elemDefCountOneShot = j.at("elemDefCountOneShot").get<int16_t>();
            fx.elemDefCountEmission = j.at("elemDefCountEmission").get<int16_t>();
            fx.totalSize = j.at("totalSize").get<int>();
            fx.msecLoopingLife = j.at("msecLoopingLife").get<int>();
            fx.msecNonLoopingLife = j.at("msecNonLoopingLife").get<int>();
            Vec3(j.at("boundingBoxDim"), fx.boundingBoxDim);
            Vec3(j.at("boundingBoxCentre"), fx.boundingBoxCentre);
            fx.occlusionQueryDepthBias = j.at("occlusionQueryDepthBias").get<float>();
            fx.occlusionQueryFadeIn = j.at("occlusionQueryFadeIn").get<int>();
            fx.occlusionQueryFadeOut = j.at("occlusionQueryFadeOut").get<int>();
            Range(j.at("occlusionQueryScaleRange"), fx.occlusionQueryScaleRange);

            const auto& jElems = j.at("elemDefs");
            const auto total = fx.elemDefCountLooping + fx.elemDefCountOneShot + fx.elemDefCountEmission;
            if (static_cast<int>(jElems.size()) != total)
                throw FxLoadError(std::format("elemDefs has {} entries, counts say {}", jElems.size(), total));
            fx.elemDefs = total > 0 ? m_memory.Alloc<FxElemDef>(static_cast<size_t>(total)) : nullptr;
            for (auto i = 0; i < total; i++)
                Elem(jElems[i], fx.elemDefs[i]);
        }

    private:
        static void Range(const json& j, FxFloatRange& r)
        {
            r.base = j.at(0).get<float>();
            r.amplitude = j.at(1).get<float>();
        }

        static void Range(const json& j, FxIntRange& r)
        {
            r.base = j.at(0).get<int>();
            r.amplitude = j.at(1).get<int>();
        }

        static void Vec3(const json& j, vec3_t& v)
        {
            v.x = j.at(0).get<float>();
            v.y = j.at(1).get<float>();
            v.z = j.at(2).get<float>();
        }

        static void Vec3Range(const json& j, FxElemVec3Range& r)
        {
            Vec3(j.at("base"), r.base);
            Vec3(j.at("amplitude"), r.amplitude);
        }

        static void VelFrame(const json& j, FxElemVelStateInFrame& f)
        {
            Vec3Range(j.at("velocity"), f.velocity);
            Vec3Range(j.at("totalDelta"), f.totalDelta);
        }

        static void VisState(const json& j, FxElemVisualState& s)
        {
            for (auto i = 0; i < 4; i++)
                s.color[i] = static_cast<char>(j.at("color").at(i).get<int>());
            s.rotationDelta = j.at("rotationDelta").get<float>();
            s.rotationTotal = j.at("rotationTotal").get<float>();
            s.size[0] = j.at("size").at(0).get<float>();
            s.size[1] = j.at("size").at(1).get<float>();
            s.scale = j.at("scale").get<float>();
        }

        const char* EffectRef(const std::string& name)
        {
            if (name.empty())
                return nullptr;
            auto* dep = m_context.LoadDependency<AssetFx>(name);
            if (!dep)
                throw FxLoadError(std::format("could not load referenced effect \"{}\"", name));
            m_registration.AddDependency(dep);
            return m_memory.Dup(name.c_str());
        }

        Material* MaterialRef(const std::string& name)
        {
            if (name.empty())
                return nullptr;
            auto* dep = m_context.LoadDependency<AssetMaterial>(name);
            if (!dep)
                throw FxLoadError(std::format("could not load material \"{}\"", name));
            m_registration.AddDependency(dep);
            return dep->Asset();
        }

        void Visual(const json& j, const FxElemDef& elem, FxElemVisuals& v)
        {
            v.anonymous = nullptr;
            switch (elem.elemType)
            {
            case FX_ELEM_TYPE_MODEL:
            {
                const auto name = j.value("model", "");
                if (name.empty())
                    break;
                auto* dep = m_context.LoadDependency<AssetXModel>(name);
                if (!dep)
                    throw FxLoadError(std::format("could not load model \"{}\"", name));
                m_registration.AddDependency(dep);
                v.model = dep->Asset();
                break;
            }
            case FX_ELEM_TYPE_RUNNER:
                v.effectDef.name = EffectRef(j.value("effect", ""));
                break;
            case FX_ELEM_TYPE_SOUND:
            {
                const auto name = j.value("sound", "");
                v.soundName = name.empty() ? nullptr : m_memory.Dup(name.c_str());
                break;
            }
            case FX_ELEM_TYPE_SPOT_LIGHT:
            {
                const auto name = j.value("lightDef", "");
                if (name.empty())
                    break;
                auto* dep = m_context.LoadDependency<AssetLightDef>(name);
                if (!dep)
                    throw FxLoadError(std::format("could not load light def \"{}\"", name));
                m_registration.AddDependency(dep);
                v.lightDef = dep->Asset();
                break;
            }
            case FX_ELEM_TYPE_OMNI_LIGHT:
                break;
            default:
                v.material = MaterialRef(j.value("material", ""));
                break;
            }
        }

        void Elem(const json& j, FxElemDef& e)
        {
            e.flags = static_cast<int>(j.at("flags").get<uint32_t>());
            e.spawn.looping.intervalMsec = j.at("spawnLooping").at(0).get<int>();
            e.spawn.looping.count = j.at("spawnLooping").at(1).get<int>();
            // spawn is a union: "spawnOneShot" is the same two ints as "spawnLooping"
            Range(j.at("spawnRange"), e.spawnRange);
            Range(j.at("fadeInRange"), e.fadeInRange);
            Range(j.at("fadeOutRange"), e.fadeOutRange);
            e.spawnFrustumCullRadius = j.at("spawnFrustumCullRadius").get<float>();
            Range(j.at("spawnDelayMsec"), e.spawnDelayMsec);
            Range(j.at("lifeSpanMsec"), e.lifeSpanMsec);
            for (auto i = 0; i < 3; i++)
            {
                Range(j.at("spawnOrigin").at(i), e.spawnOrigin[i]);
                Range(j.at("spawnAngles").at(i), e.spawnAngles[i]);
                Range(j.at("angularVelocity").at(i), e.angularVelocity[i]);
            }
            Range(j.at("spawnOffsetRadius"), e.spawnOffsetRadius);
            Range(j.at("spawnOffsetHeight"), e.spawnOffsetHeight);
            Range(j.at("initialRotation"), e.initialRotation);
            e.rotationAxis = j.at("rotationAxis").get<unsigned>();
            Range(j.at("gravity"), e.gravity);
            Range(j.at("reflectionFactor"), e.reflectionFactor);

            const auto& a = j.at("atlas");
            e.atlas.behavior = static_cast<char>(a.at("behavior").get<int>());
            e.atlas.index = static_cast<char>(a.at("index").get<int>());
            e.atlas.fps = static_cast<char>(a.at("fps").get<int>());
            e.atlas.loopCount = static_cast<char>(a.at("loopCount").get<int>());
            e.atlas.colIndexBits = static_cast<char>(a.at("colIndexBits").get<int>());
            e.atlas.rowIndexBits = static_cast<char>(a.at("rowIndexBits").get<int>());
            e.atlas.entryCountAndIndexRange = a.at("entryCountAndIndexRange").get<uint16_t>();

            e.windInfluence = j.at("windInfluence").get<float>();
            e.elemType = static_cast<FxElemType>(j.at("elemType").get<int>());
            e.visualCount = static_cast<char>(j.at("visualCount").get<int>());
            e.velIntervalCount = static_cast<char>(j.at("velIntervalCount").get<int>());
            e.visStateIntervalCount = static_cast<char>(j.at("visStateIntervalCount").get<int>());

            const auto& jVel = j.at("velSamples");
            if (jVel.size() != static_cast<size_t>(static_cast<uint8_t>(e.velIntervalCount)) + 1)
                throw FxLoadError("velSamples count does not match velIntervalCount + 1");
            e.velSamples = m_memory.Alloc<FxElemVelStateSample>(jVel.size());
            for (auto i = 0u; i < jVel.size(); i++)
            {
                VelFrame(jVel[i].at("local"), e.velSamples[i].local);
                VelFrame(jVel[i].at("world"), e.velSamples[i].world);
            }

            const auto& jVis = j.at("visSamples");
            if (jVis.empty())
                e.visSamples = nullptr;
            else
            {
                if (jVis.size() != static_cast<size_t>(static_cast<uint8_t>(e.visStateIntervalCount)) + 1)
                    throw FxLoadError("visSamples count does not match visStateIntervalCount + 1");
                e.visSamples = m_memory.Alloc<FxElemVisStateSample>(jVis.size());
                for (auto i = 0u; i < jVis.size(); i++)
                {
                    VisState(jVis[i].at("base"), e.visSamples[i].base);
                    VisState(jVis[i].at("amplitude"), e.visSamples[i].amplitude);
                }
            }

            const auto& jVisuals = j.at("visuals");
            const auto count = static_cast<uint8_t>(e.visualCount);
            if (jVisuals.size() != count)
                throw FxLoadError("visuals count does not match visualCount");
            if (e.elemType == FX_ELEM_TYPE_DECAL)
            {
                e.visuals.markArray = count > 0 ? m_memory.Alloc<FxElemMarkVisuals>(count) : nullptr;
                for (auto i = 0u; i < count; i++)
                {
                    const auto& mats = jVisuals[i].at("materials");
                    e.visuals.markArray[i].materials[0] = MaterialRef(mats.at(0).get<std::string>());
                    e.visuals.markArray[i].materials[1] = MaterialRef(mats.at(1).get<std::string>());
                }
            }
            else if (count > 1)
            {
                e.visuals.array = m_memory.Alloc<FxElemVisuals>(count);
                for (auto i = 0u; i < count; i++)
                    Visual(jVisuals[i], e, e.visuals.array[i]);
            }
            else if (count == 1)
                Visual(jVisuals[0], e, e.visuals.instance);
            else
                e.visuals.instance.anonymous = nullptr;

            Vec3(j.at("collMins"), e.collMins);
            Vec3(j.at("collMaxs"), e.collMaxs);
            e.effectOnImpact.name = EffectRef(j.at("effectOnImpact").get<std::string>());
            e.effectOnDeath.name = EffectRef(j.at("effectOnDeath").get<std::string>());
            e.effectEmitted.name = EffectRef(j.at("effectEmitted").get<std::string>());
            Range(j.at("emitDist"), e.emitDist);
            Range(j.at("emitDistVariance"), e.emitDistVariance);
            e.effectAttached.name = EffectRef(j.at("effectAttached").get<std::string>());

            e.extended.trailDef = nullptr;
            if (e.elemType == FX_ELEM_TYPE_TRAIL)
            {
                const auto& t = j.at("trail");
                if (t.is_null())
                    throw FxLoadError("trail element without trail data");
                auto* trail = m_memory.Alloc<FxTrailDef>();
                trail->scrollTimeMsec = t.at("scrollTimeMsec").get<int>();
                trail->repeatDist = t.at("repeatDist").get<int>();
                trail->splitDist = t.at("splitDist").get<int>();
                const auto& verts = t.at("verts");
                trail->vertCount = static_cast<int>(verts.size());
                trail->verts = trail->vertCount ? m_memory.Alloc<FxTrailVertex>(verts.size()) : nullptr;
                for (auto i = 0u; i < verts.size(); i++)
                {
                    trail->verts[i].pos.x = verts[i].at("pos").at(0).get<float>();
                    trail->verts[i].pos.y = verts[i].at("pos").at(1).get<float>();
                    trail->verts[i].normal.x = verts[i].at("normal").at(0).get<float>();
                    trail->verts[i].normal.y = verts[i].at("normal").at(1).get<float>();
                    trail->verts[i].texCoord = verts[i].at("texCoord").get<float>();
                }
                const auto& inds = t.at("inds");
                trail->indCount = static_cast<int>(inds.size());
                trail->inds = trail->indCount ? m_memory.Alloc<uint16_t>(inds.size()) : nullptr;
                for (auto i = 0u; i < inds.size(); i++)
                    trail->inds[i] = inds[i].get<uint16_t>();
                e.extended.trailDef = trail;
            }
            else if (e.elemType == FX_ELEM_TYPE_SPOT_LIGHT)
            {
                const auto& s = j.at("spotLight");
                if (s.is_null())
                    throw FxLoadError("spot light element without spot light data");
                auto* spot = m_memory.Alloc<FxSpotLightDef>();
                spot->fovInnerFraction = s.at("fovInnerFraction").get<float>();
                spot->startRadius = s.at("startRadius").get<float>();
                spot->endRadius = s.at("endRadius").get<float>();
                e.extended.spotLightDef = spot;
            }

            e.sortOrder = static_cast<char>(j.at("sortOrder").get<int>());
            e.lightingFrac = static_cast<char>(j.at("lightingFrac").get<int>());
            e.unused[0] = static_cast<char>(j.at("unused").at(0).get<int>());
            e.unused[1] = static_cast<char>(j.at("unused").at(1).get<int>());
            e.alphaFadeTimeMsec = j.at("alphaFadeTimeMsec").get<uint16_t>();
            e.maxWindStrength = j.at("maxWindStrength").get<uint16_t>();
            e.spawnIntervalAtMaxWind = j.at("spawnIntervalAtMaxWind").get<uint16_t>();
            e.lifespanAtMaxWind = j.at("lifespanAtMaxWind").get<uint16_t>();
            e.u.cloudDensityRange.base = std::bit_cast<int>(j.at("u").at(0).get<uint32_t>());
            e.u.cloudDensityRange.amplitude = std::bit_cast<int>(j.at("u").at(1).get<uint32_t>());
            const auto spawnSound = j.at("spawnSound").get<std::string>();
            e.spawnSound.spawnSound = spawnSound.empty() ? nullptr : m_memory.Dup(spawnSound.c_str());
            e.billboardPivot.x = j.at("billboardPivot").at(0).get<float>();
            e.billboardPivot.y = j.at("billboardPivot").at(1).get<float>();
        }

        MemoryManager& m_memory;
        AssetCreationContext& m_context;
        AssetRegistration<AssetFx>& m_registration;
    };

    class FxLoader final : public AssetCreator<AssetFx>
    {
    public:
        FxLoader(MemoryManager& memory, ISearchPath& searchPath)
            : m_memory(memory),
              m_search_path(searchPath)
        {
        }

        AssetCreationResult CreateAsset(const std::string& assetName, AssetCreationContext& context) override
        {
            const auto file = m_search_path.Open(std::format("fx/{}.w2bfx.json", assetName));
            if (!file.IsOpen())
                return AssetCreationResult::NoAction();

            auto* fx = m_memory.Alloc<FxEffectDef>();
            fx->name = m_memory.Dup(assetName.c_str());
            AssetRegistration<AssetFx> registration(assetName, fx);
            try
            {
                const auto j = json::parse(*file.m_stream);
                FxJsonReader(m_memory, context, registration).Read(j, *fx);
            }
            catch (const std::exception& e)
            {
                con::error("Failed to load fx \"{}\": {}", assetName, e.what());
                return AssetCreationResult::Failure();
            }

            return AssetCreationResult::Success(context.AddAsset(std::move(registration)));
        }

    private:
        MemoryManager& m_memory;
        ISearchPath& m_search_path;
    };
} // namespace

namespace fx
{
    std::unique_ptr<AssetCreator<AssetFx>> CreateJsonLoaderT6(MemoryManager& memory, ISearchPath& searchPath)
    {
        return std::make_unique<FxLoader>(memory, searchPath);
    }
} // namespace fx
