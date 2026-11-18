// shaderruntime.bind_material and apply_source_pass_states (see runtime.h).
#include "runtime.h"

#include "core/files.h"
#include "core/hash.h"
#include "core/pyre.h"
#include "d3d.h"
#include "materials/techsets.h"
#include "runtime_analysis.h"
#include "runtime_contracts.h"
#include "translate.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace w2b::shaders
{
    namespace fs = std::filesystem;
    using py::Json;
    using namespace runtime;

    namespace
    {
        // matching engine technique meanings (T6 slot -> WaW technique)
        const std::vector<std::pair<int, int>> SLOTS{{0, 0}, {1, 1}, {2, 4}, {3, 5}, {4, 8}, {5, 10}, {6, 12}, {7, 14}, {8, 16}, {13, 18}, {14, 20}};
        const std::set<std::string> SELF_LIT_KINDS{"unlit", "unlit_blend", "unlit_add", "unlit_distfalloff", "unlit_falloff_add", "objective"};
        const std::vector<int> REACHABLE_LIT_SLOTS{4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 19, 24, 25};
        const std::set<int> SHADOWED_LIT_SLOTS{8, 14, 19, 25};
        const std::vector<std::pair<int, int>> DLIGHT_BASE_SLOTS{{15, 4}, {16, 5}, {17, 6}, {18, 7}, {19, 8}, {24, 13}, {25, 14}};
        const std::vector<std::pair<int, int>> SPOT_SHAPE_BASE_SLOTS{{9, 7}, {10, 8}, {11, 7}, {12, 8}, {20, 7}, {21, 8}, {22, 7}, {23, 8}};
        constexpr int DEBUG_SLOTS_START = 32;
        constexpr int FLOATZ_CODE_TEXTURE = 18, TECHNIQUE_FLAG_FLOATZ = 0x40;
        constexpr double WAW_OUTPUT_SCALE = 0.5;
        const std::vector<std::string> PASS_STATE_FIELDS{"srcBlendRgb", "dstBlendRgb", "blendOpRgb", "srcBlendAlpha", "dstBlendAlpha", "blendOpAlpha", "alphaTest"};
        const std::vector<std::string> SOURCE_RASTER_FIELDS{"depthTest", "depthWrite", "polygonOffset", "cullFace", "colorWriteRgb", "colorWriteAlpha"};

        int WawSlot(const int slot)
        {
            for (const auto& [t6, waw] : SLOTS)
                if (t6 == slot)
                    return waw;
            throw std::out_of_range("slot");
        }

        bool IsSlot(const int slot)
        {
            return std::any_of(SLOTS.begin(), SLOTS.end(), [&](const auto& s) { return s.first == slot; });
        }

        long long I(const Json& v)
        {
            return v.is_number_float() ? static_cast<long long>(v.get<double>()) : v.get<long long>();
        }

        bool Truthy(const Json& v)
        {
            if (v.is_null())
                return false;
            if (v.is_boolean())
                return v.get<bool>();
            if (v.is_number())
                return v.get<double>() != 0;
            if (v.is_string())
                return !v.get<std::string>().empty();
            return !v.empty();
        }

        std::string Str(fs::path p)
        {
            return files::Utf8(p.make_preferred());
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        std::optional<fs::path> Find(const std::vector<fs::path>& roots, const fs::path& relative)
        {
            for (const auto& r : roots)
                if (IsFile(r / relative))
                    return r / relative;
            return std::nullopt;
        }

        std::string Upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        std::string TechniqueSet(const Json& material)
        {
            return material.contains("techniqueSet") && material["techniqueSet"].is_string() ? material["techniqueSet"].get<std::string>() : std::string();
        }

        using Present = std::function<bool(int)>;

        // source_slot: the WaW technique feeding T6 slot
        std::optional<int> SourceSlot(const Present& present, const std::string& sourceName, const int slot)
        {
            const int index = WawSlot(slot);
            const bool decal = sourceName.find("projected_decal") != std::string::npos;
            if (decal && slot == 2)
                for (const int projection : {WawSlot(3), WawSlot(4)})
                    if (present(projection))
                        return projection;
            if (decal && present(WawSlot(3)) && slot == 4 && !present(WawSlot(4)))
                return WawSlot(3);
            if (present(index))
                return index;
            const auto parsed = materials::ParseTechnique(sourceName);
            const bool selfLit = sourceName.rfind("mc_sky", 0) == 0 || (!parsed.lit && SELF_LIT_KINDS.count(parsed.unlit));
            if ((slot == 2 || slot == 3) && selfLit)
                for (const int alias : {4, 8})
                    if (present(alias))
                        return alias;
            return std::nullopt;
        }

        Present StatePresent(const Json& material)
        {
            const auto entries = material.value("stateBitsEntry", Json::array());
            const auto count = material.value("stateBits", Json::array()).size();
            return [entries, count](const int i) { return i < static_cast<int>(entries.size()) && I(entries[i]) >= 0 && I(entries[i]) < static_cast<long long>(count); };
        }

        const Json* SourceState(const Json& sourceMaterial, const std::string& sourceName, const int slot)
        {
            const auto index = SourceSlot(StatePresent(sourceMaterial), sourceName, slot);
            if (!index)
                return nullptr;
            return &sourceMaterial["stateBits"][static_cast<size_t>(I(sourceMaterial["stateBitsEntry"][*index]))];
        }

        std::string Get(const Json* state, const char* key, const char* fallback)
        {
            return state && state->contains(key) && (*state)[key].is_string() ? (*state)[key].get<std::string>() : std::string(fallback);
        }

        bool ScalesOutput(const Json* state)
        {
            if (!state || state->empty() || Get(state, "blendOpRgb", "disabled") == "disabled")
                return true;
            const auto src = Get(state, "srcBlendRgb", ""), dst = Get(state, "dstBlendRgb", "");
            if (src == "invdestcolor" && dst == "one")
                return true;
            return src != "destcolor" && src != "invdestcolor" && dst != "srccolor" && dst != "invsrccolor";
        }

        // _output_scale: halving a scene pass's colour (see DISPLAY_SCALE)
        bool OutputScale(const Json& sourceMaterial, const std::string& sourceName, const int slot)
        {
            const auto parsed = materials::ParseTechnique(sourceName);
            if (slot < 2 || (parsed.family == materials::Family::other && parsed.unlit == "2d"))
                return false;
            return ScalesOutput(SourceState(sourceMaterial, sourceName, slot));
        }

        std::vector<std::string> AlphaTestLines(const Json& sourceMaterial, const std::string& sourceName, const int slot, const std::string& target)
        {
            const auto rule = Get(SourceState(sourceMaterial, sourceName, slot), "alphaTest", "disabled");
            const auto a = target + ".w";
            if (rule == "gt0")
                return {"if (" + a + " <= 0.0) discard;"};
            if (rule == "ge128")
                return {"if (" + a + " < 0.5) discard;"};
            if (rule == "lt128")
                return {"if (" + a + " >= 0.5) discard;"};
            return {};
        }

        bool AlphaTested(const Json& sourceMaterial)
        {
            const auto* state = SourceState(sourceMaterial, TechniqueSet(sourceMaterial), 4);
            const auto rule = Get(state, "alphaTest", "disabled");
            return (rule == "gt0" || rule == "ge128" || rule == "lt128") && Get(state, "blendOpRgb", "disabled") == "disabled";
        }

        const Json* SourceTechnique(const Json& original, const std::string& sourceName, const int slot)
        {
            const auto& techniques = original["techniques"];
            const auto index = SourceSlot([&](const int i) { return i < static_cast<int>(techniques.size()) && Truthy(techniques[i]); }, sourceName, slot);
            return index ? &techniques[static_cast<size_t>(*index)] : nullptr;
        }

        // _write_program: content-named program and its proof
        std::string WriteProgram(const fs::path& projectRoot, const std::string& stage, const std::string& code, const fs::path& source, const Json& contract, const Json& info)
        {
            const auto digest = hash::Sha256Hex(code);
            const auto name = "waw/runtime_" + digest.substr(0, 20);
            const auto path = projectRoot / "shader_bin" / files::FromUtf8(stage + "_" + name + ".cso");
            fs::create_directories(path.parent_path());
            files::WriteBytes(path, code);
            const auto proof = projectRoot / "content_source/shaders/runtime" / files::FromUtf8(name.substr(name.rfind('/') + 1) + ".json");
            fs::create_directories(proof.parent_path());
            Json data{{"source", Str(source)}, {"bindings", contract}, {"shader", name}, {"sha256", digest}};
            for (const auto& [k, v] : info.items())
                data[k] = v;
            files::WriteText(proof, py::JsonDumps(data, 2));
            return name;
        }

        Json VertexDecl(const Json& binding, const std::map<std::string, std::pair<int, int>>& streams)
        {
            Json routing = Json::array();
            for (const auto& [_, e] : binding["inputs"].items())
            {
                const auto& [source, dest] = streams.at(Upper(e["semantic"].get<std::string>()));
                routing.push_back(Json{{"source", source}, {"dest", dest}});
            }
            const auto count = routing.size();
            for (size_t i = count; i < 16; i++)
                routing.push_back(Json{{"source", 0}, {"dest", 0}});
            return Json{{"streamCount", count}, {"hasOptionalSource", false}, {"isLoaded", false}, {"routing", routing}};
        }

        std::string TargetRegister(const Json& contract)
        {
            for (const auto& [r, e] : contract["outputs"].items())
                if (Upper(e["semantic"].get<std::string>()) == "SV_TARGET0")
                    return r;
            throw std::runtime_error("StopIteration");
        }

        Json Unique(const Json& list)
        {
            Json out = Json::array();
            for (const auto& v : list)
                if (std::find(out.begin(), out.end(), v) == out.end())
                    out.push_back(v);
            return out;
        }

        void AddVertex(const Json& binding, Json& result)
        {
            if (binding.contains("face_on_falloff") && Truthy(binding["face_on_falloff"]))
                result["face_on_falloff"] = true;
        }

        Json BindMaterialCore(const Json& sourceMaterial, Json& outputMaterial, const std::vector<fs::path>& roots, const fs::path& projectRoot, const fs::path& nativeRoot,
                              const std::optional<std::array<double, 4>>& falloffPlacement, const bool lightShadows, const bool trailGeometry)
        {
            Json result{{"active", Json::array()}, {"unsupported", Json::array()}};
            const auto sourceName = TechniqueSet(sourceMaterial);
            const auto sourceFile = Find(roots, fs::path("waw_techniquesets") / files::FromUtf8(sourceName + ".json"));
            const auto nativeName = TechniqueSet(outputMaterial);
            result["native_techset"] = nativeName;
            const auto nativeFile = nativeRoot / "techniquesets" / files::FromUtf8(nativeName + ".json");
            if (!sourceFile || !IsFile(nativeFile))
            {
                result["unsupported"].push_back("original/native technique metadata absent");
                return result;
            }
            const auto original = Json::parse(files::ReadText(*sourceFile));
            const auto pristine = Json::parse(files::ReadText(nativeFile));
            auto native = pristine;
            for (const auto& [slot, _] : SLOTS)
            {
                auto& target = native["techniques"][static_cast<size_t>(slot)];
                const auto* source = SourceTechnique(original, sourceName, slot);
                if (!Truthy(target) || !source || !Truthy(*source))
                    continue;
                if ((*source)["passes"].size() != 1 || target["passArray"].size() != 1)
                {
                    result["unsupported"].push_back("slot " + std::to_string(slot) + ": multi-pass adapter required");
                    continue;
                }
                const auto& sourcePass = (*source)["passes"][0];
                auto& targetPass = target["passArray"][0];
                const bool outputScale = OutputScale(sourceMaterial, sourceName, slot);
                const auto binary = Find(roots, fs::path("shader_bin") / files::FromUtf8("ps_" + sourcePass["pixelShader"].get<std::string>() + ".cso"));
                if (!binary)
                {
                    result["unsupported"].push_back("slot " + std::to_string(slot) + ": original program absent");
                    continue;
                }
                const auto vertexBinary = Find(roots, fs::path("shader_bin") / files::FromUtf8("vs_" + sourcePass["vertexShader"].get<std::string>() + ".cso"));
                // preferred: the original vertex+pixel pair, so the varyings stay WaW's
                std::optional<std::string> pairedError;
                if (vertexBinary)
                {
                    try
                    {
                        auto vertexAssembly = EmbedLiterals(AssemblyOf(*vertexBinary), sourcePass, "vs");
                        bool normalizedDepth;
                        std::tie(vertexAssembly, normalizedDepth) = NormalizeSoftParticleDepth(vertexAssembly, AssemblyOf(*binary));
                        auto vertexBinding = VertexContract(vertexAssembly, sourcePass, targetPass, nativeRoot, true, &sourceMaterial,
                                                            Truthy(pristine.value("worldVertFormat", Json())), trailGeometry);
                        if (normalizedDepth)
                            vertexBinding["soft_particle_depth"] = "packed WaW depth to T6 camera depth";
                        AddVertex(vertexBinding, result);
                        auto vertexTranslation = Translate(vertexAssembly, &vertexBinding);
                        const auto vertexHlsl = NativeFogAdapter(vertexTranslation.hlsl, vertexAssembly, vertexBinding, targetPass, nativeRoot);
                        const auto vertexCode = CompileHlsl(vertexHlsl, "vs_5_0");
                        Json linked = Json::array();
                        for (const auto& [_, decl] : vertexTranslation.report["outputs"].items())
                            linked.push_back(Json{{"semantic", decl[0]}, {"width", decl[1]}});
                        auto assembly = EmbedLiterals(AssemblyOf(*binary), sourcePass, "ps");
                        Json pixelPass;
                        std::tie(assembly, pixelPass) = BindLocalLightRamp(assembly, sourcePass, &sourceMaterial);
                        std::optional<std::string> visibility;
                        if (vertexBinding.contains("lighting_visibility") && vertexBinding["lighting_visibility"].is_string())
                            visibility = vertexBinding["lighting_visibility"].get<std::string>();
                        auto contract = PairedPixelContract(assembly, pixelPass, targetPass, nativeRoot, linked, visibility, &sourceMaterial, falloffPlacement, &outputMaterial);
                        if (outputScale)
                            contract["output_rgb_scale"] = WAW_OUTPUT_SCALE;
                        const auto targetReg = TargetRegister(contract);
                        const auto discard = AlphaTestLines(sourceMaterial, sourceName, slot, targetReg);
                        if (!discard.empty())
                        {
                            contract["epilogue"] = Json{{"lines", discard}, {"uses", Json::array()}};
                            if (!contract.contains("unit_conversions"))
                                contract["unit_conversions"] = Json::array();
                            contract["unit_conversions"].push_back("alpha test: discard in the program (D3D11)");
                        }
                        const auto translation = Translate(assembly, &contract);
                        const auto dxbc = CompileHlsl(translation.hlsl, "ps_5_0");
                        CheckLinkage(vertexCode, dxbc);
                        if (sourceName.rfind("mc_sky", 0) == 0)
                        {
                            // donor skies stream only POSITION; the dome also reads color and UV
                            targetPass["vertexDecl"] = VertexDecl(vertexBinding, {{"POSITION0", {0, 0}}, {"COLOR0", {1, 2}}, {"TEXCOORD0", {2, 5}}});
                            PruneMissingMaterialConstants(targetPass, outputMaterial, {&vertexBinding, &contract}, false);
                        }
                        if (materials::ParseTechnique(sourceName).unlit == "unlit_falloff_add")
                            targetPass["vertexDecl"] = VertexDecl(vertexBinding, {{"POSITION0", {0, 0}}, {"COLOR0", {1, 2}}, {"TEXCOORD0", {2, 5}}, {"NORMAL0", {3, 1}}});
                        PruneMissingMaterialConstants(targetPass, outputMaterial, {&vertexBinding, &contract}, true);
                        const auto shaderName = WriteProgram(projectRoot, "ps", dxbc, *binary, contract, translation.report);
                        const auto vertexName = WriteProgram(projectRoot, "vs", vertexCode, *vertexBinary, vertexBinding, vertexTranslation.report);
                        targetPass["pixelShader"] = Json{{"name", shaderName}};
                        targetPass["vertexShader"] = Json{{"name", vertexName}};
                        result["active"].push_back(Json{{"slot", slot},
                                                        {"source", sourcePass["pixelShader"]},
                                                        {"shader", shaderName},
                                                        {"vertex_shader", vertexName},
                                                        {"paired", true},
                                                        {"neutral", vertexBinding.value("neutral", Json::array())},
                                                        {"unit_conversions", Unique(contract.value("unit_conversions", Json::array()))},
                                                        {"lightmap", contract.value("lightmap", Json())}});
                        continue;
                    }
                    catch (const ShaderError& exc)
                    {
                        pairedError = exc.what();
                    }
                }
                try
                {
                    const auto assembly = EmbedLiterals(AssemblyOf(*binary), sourcePass, "ps");
                    auto contract = PixelContract(assembly, sourcePass, targetPass, nativeRoot, &sourceMaterial);
                    if (outputScale)
                        contract["output_rgb_scale"] = WAW_OUTPUT_SCALE;
                    const auto translation = Translate(assembly, &contract);
                    const auto dxbc = CompileHlsl(translation.hlsl, "ps_5_0");
                    const auto shaderName = WriteProgram(projectRoot, "ps", dxbc, *binary, contract, translation.report);
                    Json vertexName = nullptr;
                    if (vertexBinary)
                    {
                        try
                        {
                            const auto vertexAssembly = EmbedLiterals(AssemblyOf(*vertexBinary), sourcePass, "vs");
                            auto vertexBinding = VertexContract(vertexAssembly, sourcePass, targetPass, nativeRoot, false, &sourceMaterial, false, trailGeometry);
                            AddVertex(vertexBinding, result);
                            const auto vertexTranslation = Translate(vertexAssembly, &vertexBinding);
                            const auto vertexHlsl = NativeFogAdapter(vertexTranslation.hlsl, vertexAssembly, vertexBinding, targetPass, nativeRoot);
                            const auto vertexCode = CompileHlsl(vertexHlsl, "vs_5_0");
                            vertexName = WriteProgram(projectRoot, "vs", vertexCode, *vertexBinary, vertexBinding, vertexTranslation.report);
                        }
                        catch (const ShaderError& exc)
                        {
                            result["unsupported"].push_back("slot " + std::to_string(slot) + " vertex: " + exc.what());
                        }
                    }
                    targetPass["pixelShader"] = Json{{"name", shaderName}};
                    if (Truthy(vertexName))
                        targetPass["vertexShader"] = Json{{"name", vertexName}};
                    result["active"].push_back(Json{{"slot", slot}, {"source", sourcePass["pixelShader"]}, {"shader", shaderName}, {"vertex_shader", vertexName}});
                }
                catch (const ShaderError& exc)
                {
                    result["unsupported"].push_back("slot " + std::to_string(slot) + ": " + exc.what() + (pairedError ? " (paired: " + *pairedError + ")" : std::string()));
                }
            }
            const auto boundSlot = [&](const int slot) -> const Json*
            {
                for (const auto& a : result["active"])
                    if (I(a["slot"]) == slot)
                        return &a;
                return nullptr;
            };
            std::map<int, Json> bound;
            for (const auto& a : result["active"])
                bound[static_cast<int>(I(a["slot"]))] = a;
            // the WaW pass a projected decal draws in every slot: its lit pass, or the unlit one
            std::optional<int> decalBase;
            for (const int slot : {4, 2})
                if (bound.count(slot))
                {
                    decalBase = slot;
                    break;
                }
            if (sourceName.find("projected_decal") != std::string::npos && decalBase)
            {
                Json scratch = Json::array();
                auto& entries = outputMaterial.contains("stateBitsEntry") ? outputMaterial["stateBitsEntry"] : scratch;
                const auto& techniques = pristine["techniques"];
                for (int slot = 0; slot < std::min<int>(DEBUG_SLOTS_START, static_cast<int>(techniques.size())); slot++)
                {
                    const bool dlight = std::any_of(DLIGHT_BASE_SLOTS.begin(), DLIGHT_BASE_SLOTS.end(), [&](const auto& s) { return s.first == slot; });
                    const bool spot = std::any_of(SPOT_SHAPE_BASE_SLOTS.begin(), SPOT_SHAPE_BASE_SLOTS.end(), [&](const auto& s) { return s.first == slot; });
                    if (bound.count(slot) || !Truthy(techniques[slot]) || dlight || spot)
                        continue;
                    if (slot == 0 || slot == 1)
                    {
                        native["techniques"][static_cast<size_t>(slot)] = nullptr;
                        if (entries.is_array() && slot < static_cast<int>(entries.size()))
                            entries[static_cast<size_t>(slot)] = -1;
                        result["unsupported"].push_back("slot " + std::to_string(slot) + ": dropped (WaW projected decals cast no depth or shadow)");
                        continue;
                    }
                    native["techniques"][static_cast<size_t>(slot)] = native["techniques"][static_cast<size_t>(*decalBase)];
                    auto entry = bound[*decalBase];
                    entry["slot"] = slot;
                    entry["decal_base"] = *decalBase;
                    bound[slot] = entry;
                    result["active"].push_back(entry);
                    result["unsupported"].push_back("slot " + std::to_string(slot) + ": projected decal draws the WaW pass of slot " + std::to_string(*decalBase));
                }
            }
            for (const auto& [slot, base] : DLIGHT_BASE_SLOTS)
                if (bound.count(base) && Truthy(pristine["techniques"][static_cast<size_t>(slot)]) && !bound.count(slot))
                {
                    native["techniques"][static_cast<size_t>(slot)] = native["techniques"][static_cast<size_t>(base)];
                    auto entry = bound[base];
                    entry["slot"] = slot;
                    entry["dlight_base"] = base;
                    result["active"].push_back(entry);
                    result["unsupported"].push_back("slot " + std::to_string(slot) + ": dynamic lights not added (draws the WaW pass of slot " + std::to_string(base) + ")");
                }
            for (const auto& [slot, base] : SPOT_SHAPE_BASE_SLOTS)
                if (bound.count(base) && slot < static_cast<int>(pristine["techniques"].size()) && Truthy(pristine["techniques"][static_cast<size_t>(slot)]) && !bound.count(slot))
                {
                    native["techniques"][static_cast<size_t>(slot)] = native["techniques"][static_cast<size_t>(base)];
                    auto entry = bound[base];
                    entry["slot"] = slot;
                    entry["spot_shape_base"] = base;
                    result["active"].push_back(entry);
                    result["unsupported"].push_back("slot " + std::to_string(slot) + ": spot cookie shape not applied (draws the WaW pass of slot " + std::to_string(base) + ")");
                }
            (void)boundSlot;
            // one lightmap page per surface: WaW-lightmap passes stay only when no
            // remaining donor lit pass reads the T6-encoded lightmap
            std::vector<Json> wawLightmap;
            for (const auto& a : result["active"])
                if (a.contains("lightmap") && a["lightmap"] == "waw")
                    wawLightmap.push_back(a);
            if (!wawLightmap.empty())
            {
                std::set<int> activeSlots;
                for (const auto& a : result["active"])
                    activeSlots.insert(static_cast<int>(I(a["slot"])));
                std::vector<int> readers;
                for (const int slot : REACHABLE_LIT_SLOTS)
                {
                    if (!lightShadows && SHADOWED_LIT_SLOTS.count(slot))
                        continue;
                    if (activeSlots.count(slot) || !Truthy(pristine["techniques"][static_cast<size_t>(slot)]))
                        continue;
                    const auto& program = NativeProgram(nativeRoot, "ps", pristine["techniques"][static_cast<size_t>(slot)]["passArray"][0]);
                    bool reads = false;
                    for (const auto& [_, r] : ResourceNames(program))
                        reads |= r.name == "lightmapSamplerSecondary";
                    if (reads)
                        readers.push_back(slot);
                }
                if (!readers.empty())
                {
                    std::string list = "[";
                    for (size_t i = 0; i < readers.size(); i++)
                        list += (i ? ", " : "") + std::to_string(readers[i]);
                    list += "]";
                    for (const auto& a : wawLightmap)
                    {
                        const auto slot = static_cast<size_t>(I(a["slot"]));
                        native["techniques"][slot] = pristine["techniques"][slot];
                        auto& active = result["active"];
                        for (auto it = active.begin(); it != active.end(); ++it)
                            if (*it == a)
                            {
                                active.erase(it);
                                break;
                            }
                        result["unsupported"].push_back("slot " + std::to_string(slot) + ": WaW lightmap pass reverted; donor slots " + list + " read the T6 lightmap");
                    }
                }
                else
                    result["lightmap"] = "waw";
            }
            if (!result["active"].empty() && AlphaTested(sourceMaterial))
            {
                // the donor depth prepass / shadow caster draw the whole quad
                Json scratch = Json::array();
                auto& entries = outputMaterial.contains("stateBitsEntry") ? outputMaterial["stateBitsEntry"] : scratch;
                std::set<int> translated;
                for (const auto& a : result["active"])
                    translated.insert(static_cast<int>(I(a["slot"])));
                for (const int slot : {0, 1})
                {
                    if (translated.count(slot))
                        continue;
                    if (slot < static_cast<int>(native["techniques"].size()) && Truthy(native["techniques"][static_cast<size_t>(slot)]))
                    {
                        native["techniques"][static_cast<size_t>(slot)] = nullptr;
                        if (entries.is_array() && slot < static_cast<int>(entries.size()))
                            entries[static_cast<size_t>(slot)] = -1;
                        result["unsupported"].push_back("slot " + std::to_string(slot) + ": dropped for the WaW alpha test (cut-out casts no shadow-map shadow)");
                    }
                }
            }
            if (!result["active"].empty())
            {
                // T6 resolves the scene depth texture only for techniques flagged as reading it
                for (auto& technique : native["techniques"])
                {
                    if (!Truthy(technique))
                        continue;
                    bool floatZ = false;
                    for (const auto& p : technique["passArray"])
                        for (const auto& a : p["args"])
                            floatZ |= I(a["type"]) == 4 && a["u"].contains("value") && I(a["u"]["value"]) == FLOATZ_CODE_TEXTURE;
                    if (floatZ)
                        technique["flags"] = I(technique["flags"]) | TECHNIQUE_FLAG_FLOATZ;
                }
                // named by content, arguments included
                Json boundList = Json::array();
                for (const auto& a : result["active"])
                    boundList.push_back(Json::array({a["slot"], a["shader"], a.value("vertex_shader", Json())}));
                const auto content = py::JsonDumpsSorted(native);
                const auto tsName = "waw/runtime_" + hash::Sha256Hex(nativeName + py::JsonDumpsDefault(boundList) + content).substr(0, 16);
                for (const auto& a : result["active"])
                    native["techniques"][static_cast<size_t>(I(a["slot"]))]["name"] = tsName + "_" + std::to_string(I(a["slot"]));
                native["name"] = tsName;
                const auto path = projectRoot / "techniquesets" / files::FromUtf8(tsName + ".json");
                fs::create_directories(path.parent_path());
                files::WriteText(path, py::JsonDumps(native, 2));
                outputMaterial["techniqueSet"] = tsName;
            }
            return result;
        }
    } // namespace

    const std::string& AssemblyOf(const fs::path& path)
    {
        static std::mutex lock;
        static std::map<std::string, std::string> cache;
        const auto key = Str(path);
        {
            const std::lock_guard guard(lock);
            if (const auto it = cache.find(key); it != cache.end())
                return it->second;
        }
        std::string bytes;
        if (!files::ReadBytes(path, bytes))
            throw std::runtime_error("[Errno 2] No such file or directory: '" + key + "'");
        auto text = Disassemble(bytes);
        const std::lock_guard guard(lock);
        return cache.emplace(key, std::move(text)).first->second;
    }

    Json BindMaterial(const Json& sourceMaterial, Json& outputMaterial, const std::vector<fs::path>& roots, const fs::path& projectRoot, const fs::path& nativeRoot,
                      const std::optional<std::array<double, 4>>& falloffPlacement, const bool lightShadows, const bool trailGeometry)
    {
        auto result = BindMaterialCore(sourceMaterial, outputMaterial, roots, projectRoot, nativeRoot, falloffPlacement, lightShadows, trailGeometry);
        // original WaW cube channels are shared by translated and native readers
        if (const int count = BindNativeProbes(outputMaterial, projectRoot, nativeRoot))
            result["native_probe_passes"] = count;
        return result;
    }

    std::vector<int> ApplySourcePassStates(const Json& sourceMaterial, Json& outputMaterial, const Json& runtime)
    {
        if (!outputMaterial.contains("stateBitsEntry") || !outputMaterial.contains("stateBits"))
            return {};
        auto& entries = outputMaterial["stateBitsEntry"];
        auto& states = outputMaterial["stateBits"];
        if (!entries.is_array() || !states.is_array())
            return {};
        const auto name = TechniqueSet(sourceMaterial);
        const auto present = StatePresent(sourceMaterial);
        std::map<int, Json> active;
        for (const auto& a : runtime.value("active", Json::array()))
            active[static_cast<int>(I(a["slot"]))] = a;
        std::set<int> slots;
        for (const auto& [s, _] : SLOTS)
            slots.insert(s);
        for (const auto& [s, _] : active)
            slots.insert(s);
        std::vector<int> changed;
        for (const int slot : slots)
        {
            if (slot >= static_cast<int>(entries.size()) || I(entries[slot]) < 0 || I(entries[slot]) >= static_cast<long long>(states.size()))
                continue;
            // generated T6 light variants follow their base's provenance
            int source = slot;
            std::set<int> visited;
            while (active.count(source) && !visited.count(source))
            {
                visited.insert(source);
                const auto& entry = active[source];
                std::optional<int> base;
                for (const char* key : {"dlight_base", "spot_shape_base", "decal_base"})
                    if (entry.contains(key))
                    {
                        base = static_cast<int>(I(entry[key]));
                        break;
                    }
                if (!base)
                    break;
                source = *base;
            }
            if (!IsSlot(source))
                continue;
            const auto index = SourceSlot(present, name, source);
            if (!index)
                continue;
            const auto& wanted = sourceMaterial["stateBits"][static_cast<size_t>(I(sourceMaterial["stateBitsEntry"][*index]))];
            const auto native = states[static_cast<size_t>(I(entries[slot]))];
            const auto additive = [](const Json& s) { return Get(&s, "blendOpRgb", "disabled") != "disabled" && Get(&s, "dstBlendRgb", "") == "one"; };
            if (!active.count(slot) && !(additive(wanted) && !additive(native)))
                continue;
            auto fields = PASS_STATE_FIELDS;
            if (active.count(slot))
                fields.insert(fields.end(), SOURCE_RASTER_FIELDS.begin(), SOURCE_RASTER_FIELDS.end());
            Json update = Json::object();
            for (const auto& k : fields)
                if (wanted.contains(k))
                    update[k] = wanted[k];
            if (std::all_of(update.items().begin(), update.items().end(), [&](const auto& kv) { return native.contains(kv.key()) && native[kv.key()] == kv.value(); }))
                continue;
            auto merged = native;
            for (const auto& [k, v] : update.items())
                merged[k] = v;
            const auto sortedMerged = py::SortKeys(merged);
            std::optional<size_t> existing;
            for (size_t i = 0; i < states.size(); i++)
                if (py::SortKeys(states[i]) == sortedMerged)
                {
                    existing = i;
                    break;
                }
            if (existing)
                entries[slot] = *existing;
            else
            {
                states.push_back(merged);
                entries[slot] = states.size() - 1;
            }
            changed.push_back(slot);
        }
        return changed;
    }
} // namespace w2b::shaders
