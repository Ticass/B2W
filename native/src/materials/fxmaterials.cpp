// WaW effect materials (see fxmaterials.h), ported from fxmaterials.py.
#include "fxmaterials.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/pyre.h"
#include "gsc/tokens.h"
#include "materials/material.h"
#include "materials/techsets.h"
#include "materials/waw_material_json.h"

#include <algorithm>
#include <map>
#include <optional>

namespace w2b::materials
{
    namespace fs = std::filesystem;
    using py::Json;

    namespace
    {
        const std::vector<std::string> FAMILIES{"effect", "particle_cloud", "distortion"};
        const std::vector<std::string> T6_PREFIXES{"effect_", "particlecloud_", "distortion_"};
        const std::set<std::string> FEATURES{"zfeather", "falloff", "eyeoffset", "outdoor", "distortion", "particlecloud", "dissolve", "spotlight", "add"};
        const std::map<std::string, std::string> DROPPABLE{{"nofog", "nofog (BO2 effect techniques always fog)"},
                                                           {"zfeather", "depth feathering (no feathered BO2 variant for this family)"}};
        const std::vector<std::string> BLEND_FIELDS{"srcBlendRgb", "dstBlendRgb", "blendOpRgb", "srcBlendAlpha", "dstBlendAlpha", "blendOpAlpha",
                                                    "alphaTest", "cullFace", "colorWriteRgb", "colorWriteAlpha"};
        const std::string NONE = "\x01None"; // Python's None in a set of names

        bool StartsWith(const std::string& s, const std::string& p)
        {
            return s.rfind(p, 0) == 0;
        }

        std::string ListRepr(const std::set<std::string>& items)
        {
            std::string out = "[";
            size_t i = 0;
            for (const auto& s : items)
                out += (i++ ? ", " : "") + gsc::Repr(s);
            return out + "]";
        }

        std::pair<std::set<std::string>, std::set<std::string>> WawFeatures(const std::string& techset)
        {
            std::set<std::string> tokens;
            for (const auto part : py::Split(techset, "_"))
                tokens.emplace(part);
            std::set<std::string> feats, extra;
            if (StartsWith(techset, "particle_cloud"))
                feats.insert("particlecloud");
            else if (StartsWith(techset, "distortion"))
                feats.insert("distortion");
            for (const char* t : {"zfeather", "falloff", "eyeoffset", "outdoor", "add"})
                if (tokens.count(t))
                    feats.insert(t);
            if (tokens.count("nofog"))
                extra.insert("nofog");
            static const std::set<std::string> known{"effect", "particle", "cloud", "distortion", "scale", "zfeather", "falloff", "eyeoffset", "outdoor", "add", "nofog", "blend"};
            std::set<std::string> unknown;
            for (const auto& t : tokens)
                if (!known.count(t))
                    unknown.insert(t);
            if (!unknown.empty())
                throw TechniqueError("unknown WaW effect technique tokens " + ListRepr(unknown) + " in '" + techset + "'");
            return {feats, extra};
        }

        std::optional<std::set<std::string>> T6Features(const fs::path& raw, const std::string& techset)
        {
            const auto path = raw / "techsets" / files::FromUtf8(techset + ".techset");
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                return std::nullopt;
            static const pyre::Regex technique("(pimp_technique_[a-z0-9_]+);");
            static const pyre::Regex argument("(?:material|code|sampler)\\.(\\w+)");
            static const pyre::Regex shader("Shader [\\d.]+ \"pimp_shader_([a-z_]+?)_[0-9a-f]+\\.hlsl\"");
            std::set<std::string> feats;
            for (const auto& m : technique.FindAll(encoding::Utf8Replace(bytes)))
            {
                const auto& tech = m[0];
                if (tech.find("wireframe") != std::string::npos || tech.find("debugperformance") != std::string::npos)
                    continue;
                if (tech.find("effectadd") != std::string::npos)
                    feats.insert("add");
                std::string source;
                if (!files::ReadBytes(raw / "techniques" / files::FromUtf8(tech + ".tech"), source))
                    return std::nullopt;
                source = encoding::Utf8Replace(source);
                std::set<std::string> args, shaders;
                for (const auto& a : argument.FindAll(source))
                    args.insert(a[0]);
                for (const auto& s : shader.FindAll(source))
                    shaders.insert(s[0]);
                for (const auto& [arg, feature] : std::vector<std::pair<std::string, std::string>>{{"featherParms", "zfeather"}, {"falloffParms", "falloff"}, {"eyeOffsetParms", "eyeoffset"},
                                                                                                  {"outdoor", "outdoor"}, {"alphaDissolveParms", "dissolve"}, {"spotLightWeight", "spotlight"}})
                    if (args.count(arg))
                        feats.insert(feature);
                if (shaders.count("distortion"))
                    feats.insert("distortion");
                if (shaders.count("particlecloud"))
                    feats.insert("particlecloud");
            }
            return feats;
        }

        const Json* MainState(const Json& material)
        {
            if (!material.contains("stateBits"))
                return nullptr;
            for (const auto& s : material["stateBits"])
            {
                const bool line = s.contains("polymodeLine") && s["polymodeLine"].is_boolean() && s["polymodeLine"].get<bool>();
                if (!line && !(s.contains("depthTest") && s["depthTest"] == "disabled"))
                    return &s;
            }
            return nullptr;
        }

        std::string BlendClass(const Json* state)
        {
            const auto get = [&](const char* k) { return state->contains(k) && (*state)[k].is_string() ? (*state)[k].get<std::string>() : std::string("None"); };
            if (!state || state->empty() || get("blendOpRgb") == "disabled")
                return "opaque";
            const auto src = get("srcBlendRgb"), dst = get("dstBlendRgb");
            if (dst == "one")
                return "add";
            if (dst == "invsrcalpha")
                return src == "one" ? "premult" : "blend";
            if (src == "destcolor" || src == "zero" || dst == "srccolor")
                return "multiply";
            return src + "/" + dst;
        }

        std::set<std::string> Slots(const Json& material, const bool imagesOnly)
        {
            std::set<std::string> out;
            if (material.contains("textures"))
                for (const auto& t : material["textures"])
                {
                    if (imagesOnly && !(t.contains("image") && t["image"].is_string() && !t["image"].get<std::string>().empty()))
                        continue;
                    out.insert(t.contains("name") && t["name"].is_string() ? t["name"].get<std::string>() : NONE);
                }
            return out;
        }
    } // namespace

    bool IsEffectTechset(const std::string& t4Techset)
    {
        return std::any_of(FAMILIES.begin(), FAMILIES.end(), [&](const std::string& f) { return StartsWith(t4Techset, f); });
    }

    std::vector<EffectDonor> IndexEffectDonors(const fs::path& stockMaterials, const fs::path* techsetDump, const fs::path& bo2Raw)
    {
        auto paths = files::RGlob(stockMaterials, ".json");
        std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b)
                  {
                      auto x = files::Utf8(fs::path(a).make_preferred()), y = files::Utf8(fs::path(b).make_preferred());
                      std::transform(x.begin(), x.end(), x.begin(), ::tolower);
                      std::transform(y.begin(), y.end(), y.begin(), ::tolower);
                      return x < y;
                  });
        std::vector<EffectDonor> donors;
        std::map<std::string, std::optional<std::set<std::string>>> cache;
        for (const auto& path : paths)
        {
            Json data;
            try
            {
                data = Json::parse(files::ReadText(path));
            }
            catch (const std::exception&)
            {
                continue;
            }
            const auto ts = data.value("techniqueSet", std::string());
            if (std::none_of(T6_PREFIXES.begin(), T6_PREFIXES.end(), [&](const std::string& p) { return StartsWith(ts, p); }))
                continue;
            std::error_code e;
            if (techsetDump && !fs::exists(*techsetDump / "techniquesets" / files::FromUtf8(ts + ".json"), e))
                continue;
            if (!cache.count(ts))
                cache[ts] = T6Features(bo2Raw, ts);
            if (!cache[ts])
                continue;
            donors.push_back(EffectDonor{ts, path, data, *cache[ts], BlendClass(MainState(data))});
        }
        return donors;
    }

    std::pair<const EffectDonor*, std::vector<std::string>> ChooseEffectDonor(const Json& t4, const std::vector<EffectDonor>& donors)
    {
        const auto techset = t4.value("techniqueSet", std::string());
        const auto [feats, extra] = WawFeatures(techset);
        const auto wantBlend = BlendClass(MainState(t4));
        auto slots = Slots(t4, true);
        slots.insert(NONE);
        std::vector<std::string> notes;
        for (const auto& f : extra)
            notes.push_back("dropped: " + DROPPABLE.at(f));
        std::set<std::string> noFeather = feats;
        noFeather.erase("zfeather");
        const auto withoutAdd = [](std::set<std::string> s)
        {
            s.erase("add");
            return s;
        };
        for (const auto& [required, note] : {std::pair<std::set<std::string>, std::optional<std::string>>{feats, std::nullopt}, {noFeather, DROPPABLE.at("zfeather")}})
        {
            std::vector<const EffectDonor*> pool;
            for (const auto& d : donors)
            {
                std::set<std::string> shared;
                for (const auto& f : d.features)
                    if (FEATURES.count(f) && f != "add")
                        shared.insert(f);
                if (shared != withoutAdd(required))
                    continue;
                const auto donorSlots = Slots(d.material, false);
                if (!std::includes(slots.begin(), slots.end(), donorSlots.begin(), donorSlots.end()))
                    continue;
                pool.push_back(&d);
            }
            if (pool.empty())
                continue;
            // same blend class first, then the additive variant when WaW's is additive
            std::stable_sort(pool.begin(), pool.end(), [&](const EffectDonor* a, const EffectDonor* b)
                             {
                                 const auto key = [&](const EffectDonor* d)
                                 { return std::tuple(d->blend != wantBlend, d->features.count("add") != feats.count("add"), d->techset, files::Utf8(fs::path(d->path).make_preferred())); };
                                 return key(a) < key(b);
                             });
            const auto* best = pool[0];
            if (note && required != feats)
                notes.push_back("dropped: " + *note);
            if (best->blend != wantBlend)
                notes.push_back("donor blend " + best->blend + ", WaW blend " + wantBlend + " (WaW blend state applied)");
            return {best, notes};
        }
        throw TechniqueError("no stock BO2 effect technique set with features " + ListRepr(feats) + " for '" + techset + "'");
    }

    Json BuildEffectMaterial(const Json& t4, const EffectDonor& donor, std::vector<std::string>& notes)
    {
        auto out = BuildMaterial(FromOatJson(t4), donor.material, notes);
        if (const auto* main = MainState(t4); main && out.contains("stateBits"))
            for (auto& state : out["stateBits"])
            {
                if (state.contains("polymodeLine") && state["polymodeLine"].is_boolean() && state["polymodeLine"].get<bool>())
                    continue;
                for (const auto& k : BLEND_FIELDS)
                    if (main->contains(k))
                        state[k] = (*main)[k];
            }
        if (t4.contains("textureAtlas"))
            out["textureAtlas"] = t4["textureAtlas"];
        return out;
    }
} // namespace w2b::materials
