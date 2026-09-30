#include "FxJsonDumperT6.h"

#include <bit>
#include <format>
#include <iomanip>
#include <nlohmann/json.hpp>

using namespace nlohmann;
using namespace T6;

namespace
{
    std::string RefName(const char* name)
    {
        if (!name)
            return "";
        if (name[0] == ',')
            return name + 1;
        return name;
    }

    json Range(const FxFloatRange& r)
    {
        return json::array({r.base, r.amplitude});
    }

    json Range(const FxIntRange& r)
    {
        return json::array({r.base, r.amplitude});
    }

    json Vec3(const vec3_t& v)
    {
        return json::array({v.x, v.y, v.z});
    }

    json Vec3Range(const FxElemVec3Range& r)
    {
        return json{{"base", Vec3(r.base)}, {"amplitude", Vec3(r.amplitude)}};
    }

    json VelFrame(const FxElemVelStateInFrame& f)
    {
        return json{{"velocity", Vec3Range(f.velocity)}, {"totalDelta", Vec3Range(f.totalDelta)}};
    }

    json VisState(const FxElemVisualState& s)
    {
        return json{
            {"color",
             json::array({static_cast<uint8_t>(s.color[0]), static_cast<uint8_t>(s.color[1]), static_cast<uint8_t>(s.color[2]), static_cast<uint8_t>(s.color[3])})},
            {"rotationDelta", s.rotationDelta},
            {"rotationTotal", s.rotationTotal},
            {"size", json::array({s.size[0], s.size[1]})},
            {"scale", s.scale},
        };
    }

    json Visual(const FxElemDef& elem, const FxElemVisuals& v)
    {
        switch (elem.elemType)
        {
        case FX_ELEM_TYPE_MODEL:
            return json{{"model", v.model ? RefName(v.model->name) : ""}};
        case FX_ELEM_TYPE_RUNNER:
            return json{{"effect", RefName(v.effectDef.name)}};
        case FX_ELEM_TYPE_SOUND:
            return json{{"sound", v.soundName ? v.soundName : ""}};
        case FX_ELEM_TYPE_SPOT_LIGHT:
            return json{{"lightDef", v.lightDef ? RefName(v.lightDef->name) : ""}};
        case FX_ELEM_TYPE_OMNI_LIGHT:
            return json::object();
        default:
            return json{{"material", v.material ? RefName(v.material->info.name) : ""}};
        }
    }

    json Elem(const FxElemDef& e)
    {
        json j;
        j["flags"] = static_cast<uint32_t>(e.flags);
        j["spawnLooping"] = json::array({e.spawn.looping.intervalMsec, e.spawn.looping.count});
        j["spawnOneShot"] = Range(e.spawn.oneShot.count);
        j["spawnRange"] = Range(e.spawnRange);
        j["fadeInRange"] = Range(e.fadeInRange);
        j["fadeOutRange"] = Range(e.fadeOutRange);
        j["spawnFrustumCullRadius"] = e.spawnFrustumCullRadius;
        j["spawnDelayMsec"] = Range(e.spawnDelayMsec);
        j["lifeSpanMsec"] = Range(e.lifeSpanMsec);
        j["spawnOrigin"] = json::array({Range(e.spawnOrigin[0]), Range(e.spawnOrigin[1]), Range(e.spawnOrigin[2])});
        j["spawnOffsetRadius"] = Range(e.spawnOffsetRadius);
        j["spawnOffsetHeight"] = Range(e.spawnOffsetHeight);
        j["spawnAngles"] = json::array({Range(e.spawnAngles[0]), Range(e.spawnAngles[1]), Range(e.spawnAngles[2])});
        j["angularVelocity"] = json::array({Range(e.angularVelocity[0]), Range(e.angularVelocity[1]), Range(e.angularVelocity[2])});
        j["initialRotation"] = Range(e.initialRotation);
        j["rotationAxis"] = e.rotationAxis;
        j["gravity"] = Range(e.gravity);
        j["reflectionFactor"] = Range(e.reflectionFactor);
        j["atlas"] = json{
            {"behavior", static_cast<uint8_t>(e.atlas.behavior)},
            {"index", static_cast<uint8_t>(e.atlas.index)},
            {"fps", static_cast<uint8_t>(e.atlas.fps)},
            {"loopCount", static_cast<uint8_t>(e.atlas.loopCount)},
            {"colIndexBits", static_cast<uint8_t>(e.atlas.colIndexBits)},
            {"rowIndexBits", static_cast<uint8_t>(e.atlas.rowIndexBits)},
            {"entryCountAndIndexRange", e.atlas.entryCountAndIndexRange},
        };
        j["windInfluence"] = e.windInfluence;
        j["elemType"] = static_cast<uint8_t>(e.elemType);
        j["visualCount"] = static_cast<uint8_t>(e.visualCount);
        j["velIntervalCount"] = static_cast<uint8_t>(e.velIntervalCount);
        j["visStateIntervalCount"] = static_cast<uint8_t>(e.visStateIntervalCount);

        auto vel = json::array();
        for (auto i = 0; e.velSamples && i <= static_cast<uint8_t>(e.velIntervalCount); i++)
            vel.push_back(json{{"local", VelFrame(e.velSamples[i].local)}, {"world", VelFrame(e.velSamples[i].world)}});
        j["velSamples"] = vel;

        auto vis = json::array();
        for (auto i = 0; e.visSamples && i <= static_cast<uint8_t>(e.visStateIntervalCount); i++)
            vis.push_back(json{{"base", VisState(e.visSamples[i].base)}, {"amplitude", VisState(e.visSamples[i].amplitude)}});
        j["visSamples"] = vis;

        auto visuals = json::array();
        const auto count = static_cast<uint8_t>(e.visualCount);
        if (e.elemType == FX_ELEM_TYPE_DECAL)
        {
            for (auto i = 0; e.visuals.markArray && i < count; i++)
            {
                const auto& m = e.visuals.markArray[i];
                visuals.push_back(json{
                    {"materials",
                     json::array({m.materials[0] ? RefName(m.materials[0]->info.name) : "", m.materials[1] ? RefName(m.materials[1]->info.name) : ""})},
                });
            }
        }
        else if (count > 1)
        {
            for (auto i = 0; e.visuals.array && i < count; i++)
                visuals.push_back(Visual(e, e.visuals.array[i]));
        }
        else if (count == 1)
            visuals.push_back(Visual(e, e.visuals.instance));
        j["visuals"] = visuals;

        j["collMins"] = Vec3(e.collMins);
        j["collMaxs"] = Vec3(e.collMaxs);
        j["effectOnImpact"] = RefName(e.effectOnImpact.name);
        j["effectOnDeath"] = RefName(e.effectOnDeath.name);
        j["effectEmitted"] = RefName(e.effectEmitted.name);
        j["emitDist"] = Range(e.emitDist);
        j["emitDistVariance"] = Range(e.emitDistVariance);
        j["effectAttached"] = RefName(e.effectAttached.name);

        j["trail"] = nullptr;
        j["spotLight"] = nullptr;
        if (e.elemType == FX_ELEM_TYPE_TRAIL && e.extended.trailDef)
        {
            const auto& t = *e.extended.trailDef;
            auto verts = json::array();
            for (auto i = 0; t.verts && i < t.vertCount; i++)
                verts.push_back(json{
                    {"pos", json::array({t.verts[i].pos.x, t.verts[i].pos.y})},
                    {"normal", json::array({t.verts[i].normal.x, t.verts[i].normal.y})},
                    {"texCoord", t.verts[i].texCoord},
                });
            auto inds = json::array();
            for (auto i = 0; t.inds && i < t.indCount; i++)
                inds.push_back(t.inds[i]);
            j["trail"] = json{
                {"scrollTimeMsec", t.scrollTimeMsec},
                {"repeatDist", t.repeatDist},
                {"splitDist", t.splitDist},
                {"verts", verts},
                {"inds", inds},
            };
        }
        else if (e.elemType == FX_ELEM_TYPE_SPOT_LIGHT && e.extended.spotLightDef)
        {
            const auto& s = *e.extended.spotLightDef;
            j["spotLight"] = json{
                {"fovInnerFraction", s.fovInnerFraction},
                {"startRadius", s.startRadius},
                {"endRadius", s.endRadius},
            };
        }

        j["sortOrder"] = static_cast<uint8_t>(e.sortOrder);
        j["lightingFrac"] = static_cast<uint8_t>(e.lightingFrac);
        j["unused"] = json::array({static_cast<uint8_t>(e.unused[0]), static_cast<uint8_t>(e.unused[1])});
        j["alphaFadeTimeMsec"] = e.alphaFadeTimeMsec;
        j["maxWindStrength"] = e.maxWindStrength;
        j["spawnIntervalAtMaxWind"] = e.spawnIntervalAtMaxWind;
        j["lifespanAtMaxWind"] = e.lifespanAtMaxWind;
        // union: billboard trim (floats) or cloud density range (ints); raw bits
        j["u"] = json::array({std::bit_cast<uint32_t>(e.u.cloudDensityRange.base), std::bit_cast<uint32_t>(e.u.cloudDensityRange.amplitude)});
        j["spawnSound"] = e.spawnSound.spawnSound ? e.spawnSound.spawnSound : "";
        j["billboardPivot"] = json::array({e.billboardPivot.x, e.billboardPivot.y});
        return j;
    }
} // namespace

namespace fx
{
    JsonDumperT6::JsonDumperT6(const AssetPool<AssetFx::Type>& pool)
        : AbstractAssetDumper(pool)
    {
    }

    void JsonDumperT6::DumpAsset(AssetDumpingContext& context, const XAssetInfo<AssetFx::Type>& asset)
    {
        const auto* fx = asset.Asset();
        const auto assetFile = context.OpenAssetFile(std::format("fx/{}.w2bfx.json", asset.m_name));
        if (!assetFile)
            return;

        json j;
        j["_type"] = "waw2bo2_fx";
        j["_game"] = "T6";
        j["_version"] = 1;
        j["name"] = asset.m_name;
        j["flags"] = fx->flags;
        j["efPriority"] = static_cast<uint8_t>(fx->efPriority);
        j["elemDefCountLooping"] = fx->elemDefCountLooping;
        j["elemDefCountOneShot"] = fx->elemDefCountOneShot;
        j["elemDefCountEmission"] = fx->elemDefCountEmission;
        j["totalSize"] = fx->totalSize;
        j["msecLoopingLife"] = fx->msecLoopingLife;
        j["msecNonLoopingLife"] = fx->msecNonLoopingLife;
        j["boundingBoxDim"] = Vec3(fx->boundingBoxDim);
        j["boundingBoxCentre"] = Vec3(fx->boundingBoxCentre);
        j["occlusionQueryDepthBias"] = fx->occlusionQueryDepthBias;
        j["occlusionQueryFadeIn"] = fx->occlusionQueryFadeIn;
        j["occlusionQueryFadeOut"] = fx->occlusionQueryFadeOut;
        j["occlusionQueryScaleRange"] = Range(fx->occlusionQueryScaleRange);

        auto elems = json::array();
        const auto total = fx->elemDefCountLooping + fx->elemDefCountOneShot + fx->elemDefCountEmission;
        for (auto i = 0; fx->elemDefs && i < total; i++)
            elems.push_back(Elem(fx->elemDefs[i]));
        j["elemDefs"] = elems;

        *assetFile << std::setw(1) << std::setfill('\t') << j << "\n";
    }
} // namespace fx
