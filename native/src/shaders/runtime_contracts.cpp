// Program contracts (see runtime_contracts.h), ported from shaderruntime.py.
#include "runtime_contracts.h"

#include "core/files.h"
#include "core/pyre.h"
#include "d3d.h"
#include "gsc/tokens.h"
#include "materials/techsets.h"
#include "materials/waw_material_json.h"
#include "runtime.h"
#include "runtime_analysis.h"
#include "translate.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <set>

namespace w2b::shaders::runtime
{
    namespace
    {
        // WaW code uniforms whose T6 counterpart has another name (measured pairs)
        const std::map<std::string, std::string> CODE_UNIFORM_ALIASES{{"baseLightingCoords", "gridLightingCoordsAndVis"}, {"sunSpecular", "sunDiffuse"}};
        // WaW code matrices T6 binds only the inverse of
        const std::map<std::string, std::string> AFFINE_INVERSES{{"inverseWorldMatrix", "worldMatrix"}, {"viewMatrix", "inverseViewMatrix"}};
        const std::map<std::string, std::string> NEUTRAL_CODE_UNIFORMS{{"destructibleParms", "float4(0, 0, 0, 0)"}, {"__characterCharredAmount", "float4(0, 0, 0, 0)"}};
        const std::vector<std::string> PACKED_VECTOR_INPUTS{"NORMAL0", "TEXCOORD2"};
        const std::vector<std::string> LAYER_STREAM_INPUTS{"TEXCOORD3", "TEXCOORD4", "TEXCOORD5", "TEXCOORD6"};
        constexpr double DISPLAY_SCALE = 4.0;
        const std::map<std::string, std::pair<int, std::string>> WORLD_SURFACE_TEXTURE_SLOTS{{"lightmapSamplerSecondary", {13, "2d"}}, {"reflectionProbeSampler", {15, "cube"}}};
        const std::map<std::string, int> CUSTOM_SAMPLER_BITS{{"reflectionProbeSampler", 1}, {"lightmapSamplerSecondary", 2}};
        const std::set<std::string> PER_SURFACE_SAMPLERS{"lightmapSamplerPrimary", "lightmapSamplerSecondary", "reflectionProbeSampler"};
        const std::map<std::string, std::string> LIGHT_ADAPTERS{
            {"lightPosition", "lightPosition: T6 xyz, w = 1/radius from lightFallOffA.w"},
            {"lightDiffuse", "lightDiffuse: sqrt(4 hdr * linear)"},
            {"lightSpecular", "lightSpecular: light colour, sqrt(4 hdr * linear)"},
            {"lightSpotDir", "lightSpotDir: T6 xyz"},
            {"lightSpotFactors", "lightSpotFactors: from lightFallOffA/B (staged aAbB)"},
            {"lightFalloffPlacement", "lightFalloffPlacement: light def lightmap lookup row"},
            {"spotShadowmapPixelAdjust", "spotShadowmapPixelAdjust: WaW tap pattern in T6 tile units (derived)"}};
        const std::map<std::string, int> LIGHT_CODE_CONSTANTS{{"lightPosition", 0}, {"lightDiffuse", 1}, {"lightSpotDir", 2}, {"lightSpotFactors", 3},
                                                              {"lightFallOffA", 5}, {"lightFallOffB", 6}, {"spotShadowmapPixelAdjust", 60}};
        constexpr double WAW_SPOT_SHADOW_TAPS[4] = {0.25, 0.25, 0.5, -0.125};
        const std::map<std::string, int> CODE_TEXTURE_ARGS{{"attenuationSampler", 15}, {"floatZSampler", 18}, {"refractSampler", 9}};
        const std::map<std::string, std::pair<double, double>> WAW_PAGE_UV{{"lightmapSamplerSecondary", {2.0 / 3.0, 0.0}}, {"lightmapSamplerPrimary", {1.0 / 3.0, 2.0 / 3.0}}};
        constexpr double SCORCH_NEUTRAL[4] = {0.0, 0.0, 0.0, 0.0};
        constexpr double FACE_ON_FALLOFF_PARMS[4] = {0.0, 0.0, 0.0, 1.0};

        std::string F(const double v)
        {
            return py::FloatRepr(v);
        }

        long long I(const Json& v)
        {
            return v.is_number_float() ? static_cast<long long>(v.get<double>()) : v.get<long long>();
        }

        long long Value(const Json& arg)
        {
            return arg.contains("u") ? (arg["u"].contains("value") ? I(arg["u"]["value"]) : -1) : I(arg["value"]);
        }

        bool Contains(const std::string& haystack, const std::string& needle)
        {
            return haystack.find(needle) != std::string::npos;
        }

        bool StartsWith(const std::string& s, const std::string& p)
        {
            return s.rfind(p, 0) == 0;
        }

        std::string Upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        std::string Escape(const std::string& s)
        {
            return pyre::Escape(s);
        }

        // re.search(pattern, text, flags) is not None, memoized: the same native
        // donor programs are searched with the same patterns for every material
        bool Search(const std::string& pattern, const std::string& text, unsigned flags = 0)
        {
            static std::mutex lock;
            static std::unordered_map<std::string, bool> found;
            const auto key = std::to_string(flags) + '\x1f' + pattern + '\x1f' + text;
            {
                const std::lock_guard guard(lock);
                if (const auto it = found.find(key); it != found.end())
                    return it->second;
            }
            const bool result = pyre::Regex(pattern, flags).Search(text).has_value();
            const std::lock_guard guard(lock);
            found.emplace(key, result);
            return result;
        }

        // shaders.translate(assembly): the unbound report
        Json Ir(const std::string& assembly)
        {
            static std::mutex lock;
            static std::unordered_map<std::string, Json> cache;
            {
                const std::lock_guard guard(lock);
                if (const auto it = cache.find(assembly); it != cache.end())
                    return it->second;
            }
            auto report = Translate(assembly, nullptr).report;
            const std::lock_guard guard(lock);
            return cache.emplace(assembly, std::move(report)).first->second;
        }

        std::vector<std::string> Keys(const Json& value)
        {
            std::vector<std::string> out;
            if (value.is_object())
                for (const auto& [k, _] : value.items())
                    out.push_back(k);
            else
                for (const auto& v : value)
                    out.push_back(v.get<std::string>());
            return out;
        }

        // source pass arguments by register: c<dest+row> for types 5, 6, 7
        std::map<std::string, Json> ConstantArgs(const Json& sourcePass)
        {
            std::map<std::string, Json> out;
            for (const auto& a : sourcePass["args"])
            {
                const auto type = I(a["type"]);
                if (type == 5 || type == 6 || type == 7)
                    for (long long row = 0; row < (a.contains("rowCount") ? I(a["rowCount"]) : 1); row++)
                        out["c" + std::to_string(I(a["dest"]) + row)] = a;
            }
            return out;
        }

        const Json* FindArg(const Json& pass, const long long type, const long long value)
        {
            for (const auto& a : pass["args"])
                if (I(a["type"]) == type && a["u"].contains("value") && I(a["u"]["value"]) == value)
                    return &a;
            return nullptr;
        }

        std::optional<std::vector<double>> MaterialLiteral(const materials::WawMaterial& sourceMaterial, const long long hash)
        {
            for (const auto& constant : sourceMaterial.constants)
            {
                std::optional<long long> key;
                if (constant.nameHash)
                    key = *constant.nameHash;
                else if (constant.name && !constant.name->empty())
                    key = WawHash(*constant.name);
                if (key && *key == hash)
                    return std::vector<double>(constant.literal.begin(), constant.literal.end());
            }
            return std::nullopt;
        }

        std::string Float4(const std::vector<double>& v)
        {
            std::string out = "float4(";
            for (size_t i = 0; i < v.size(); i++)
                out += (i ? ", " : "") + F(v[i]);
            return out + ")";
        }

        void AddConversion(Json& contract, const std::string& text)
        {
            if (!contract.contains("unit_conversions"))
                contract["unit_conversions"] = Json::array();
            contract["unit_conversions"].push_back(text);
        }

        void AddTo(Json& contract, const char* key, const Json& value)
        {
            if (!contract.contains(key))
                contract[key] = Json::array();
            contract[key].push_back(value);
        }

        std::string HdrDisplay(const int row)
        {
            return "(" + F(DISPLAY_SCALE) + " * t6_cb0[" + std::to_string(row) + "].x)";
        }

        bool NativeSqrtHdrOutput(const std::string& ps)
        {
            return Search("float4 hdrControl0;\\s*// Offset:\\s*320 Size:\\s*16\\s*\\n", ps) && Search("^sqrt o0\\.xyz", ps, pyre::MULTILINE);
        }

        void EnsureCodeConstantArg(Json& nativePass, const std::string& field, const int location)
        {
            const long long value = 0x01000000 + LIGHT_CODE_CONSTANTS.at(field);
            for (const auto& a : nativePass["args"])
                if (I(a["type"]) == 5 && I(a["u"]["value"]) == value)
                    return;
            for (const auto& a : nativePass["args"])
                if (I(a["type"]) == 5 && I(a["location"]) == location)
                    throw ShaderError("native constant row of " + field + " carries another code constant");
            const auto start = static_cast<size_t>(I(nativePass["perPrimArgCount"]) + I(nativePass["perObjArgCount"]));
            auto& args = nativePass["args"];
            size_t position = args.size();
            for (size_t i = start; i < args.size(); i++)
                if (I(args[i]["type"]) == 5 && I(args[i]["location"]) > location)
                {
                    position = i;
                    break;
                }
            args.insert(args.begin() + static_cast<long long>(position), Json{{"buffer", 0}, {"location", location}, {"size", 16}, {"type", 5}, {"u", Json{{"value", value}}}});
            nativePass["stableArgCount"] = I(nativePass["stableArgCount"]) + 1;
        }

        Json LightConstant(const std::string& name, const std::map<std::string, Field>& nativeFields, const std::optional<int>& hdrRow,
                           const std::optional<std::array<double, 4>>& falloffPlacement, Json* nativePass)
        {
            const auto row = [&](const std::string& field)
            {
                const auto it = nativeFields.find(field);
                if (it == nativeFields.end() || it->second.slot != 0 || it->second.offset % 16)
                    throw ShaderError("native light uniform absent: " + field);
                if (nativePass)
                    EnsureCodeConstantArg(*nativePass, field, it->second.offset);
                return it->second.offset / 16;
            };
            const auto use = [](const int r) { return Json::array({0, r}); };
            if (name == "lightFalloffPlacement")
            {
                if (!falloffPlacement)
                    throw ShaderError("lightFalloffPlacement needs one light def shared by all local lights");
                return Json{{"expr", Float4({(*falloffPlacement)[0], (*falloffPlacement)[1], (*falloffPlacement)[2], (*falloffPlacement)[3]})}, {"uses", Json::array()}};
            }
            if (name == "lightDiffuse" || name == "lightSpecular")
            {
                if (!hdrRow)
                    throw ShaderError("native light colour units not recognized");
                const int diffuse = row("lightDiffuse");
                return Json{{"expr", "float4(sqrt(max(t6_cb0[" + std::to_string(diffuse) + "].xyz * " + HdrDisplay(*hdrRow) + ", 0)), 1.0)"},
                            {"uses", Json::array({use(diffuse), use(*hdrRow)})}};
            }
            if (name == "spotShadowmapPixelAdjust")
            {
                const int adjust = row("spotShadowmapPixelAdjust");
                const auto& t = WAW_SPOT_SHADOW_TAPS;
                return Json{{"expr", "(t6_cb0[" + std::to_string(adjust) + "].xyxy * float4(" + F(t[0]) + ", " + F(t[1]) + ", " + F(t[2]) + ", " + F(t[3]) + "))"},
                            {"uses", Json::array({use(adjust)})}};
            }
            if (name == "lightSpotDir")
            {
                const int spot = row("lightSpotDir");
                return Json{{"expr", "float4(t6_cb0[" + std::to_string(spot) + "].xyz, 0.0)"}, {"uses", Json::array({use(spot)})}};
            }
            if (name == "lightPosition")
            {
                const int position = row("lightPosition"), a = row("lightFallOffA");
                return Json{{"expr", "float4(t6_cb0[" + std::to_string(position) + "].xyz, -t6_cb0[" + std::to_string(a) + "].w)"},
                            {"uses", Json::array({use(position), use(a)})}};
            }
            const int a = row("lightFallOffA"), b = row("lightFallOffB"), factors = row("lightSpotFactors");
            return Json{{"expr", "float4(t6_cb0[" + std::to_string(a) + "].x, t6_cb0[" + std::to_string(b) + "].x, t6_cb0[" + std::to_string(a) + "].y, t6_cb0[" +
                                     std::to_string(factors) + "].w)"},
                        {"uses", Json::array({use(a), use(b), use(factors)})}};
        }

        Json AffineInverseRow(const Field& field, const int row)
        {
            if (row >= 3)
                return Json{{"expr", "float4(0, 0, 0, 1)"}, {"uses", Json::array()}};
            std::string m[3];
            for (int i = 0; i < 3; i++)
                m[i] = "t6_cb" + std::to_string(field.slot) + "[" + std::to_string(field.offset / 16 + i) + "]";
            std::string columns[3];
            for (int c = 0; c < 3; c++)
            {
                const char ch = "xyz"[c];
                columns[c] = "float3(" + m[0] + "." + ch + ", " + m[1] + "." + ch + ", " + m[2] + "." + ch + ")";
            }
            const auto translation = "float3(" + m[0] + ".w, " + m[1] + ".w, " + m[2] + ".w)";
            const auto cofactor = "cross(" + columns[(row + 1) % 3] + ", " + columns[(row + 2) % 3] + ")";
            const auto determinant = "dot(" + columns[0] + ", cross(" + columns[1] + ", " + columns[2] + "))";
            Json uses = Json::array();
            for (int i = 0; i < 3; i++)
                uses.push_back(Json::array({field.slot, field.offset / 16 + i}));
            return Json{{"expr", "float4(" + cofactor + ", -dot(" + cofactor + ", " + translation + ")) / " + determinant}, {"uses", uses}};
        }

        // native_depth_reference: a stock program proving the scene-depth units
        Field NativeDepthReference(const fs::path& nativeRoot)
        {
            static std::mutex lock;
            static std::map<std::string, Field> cache;
            const auto key = files::Utf8(nativeRoot);
            {
                const std::lock_guard guard(lock);
                if (const auto it = cache.find(key); it != cache.end())
                    return it->second;
            }
            std::vector<fs::path> programs;
            std::error_code e;
            for (const auto& entry : fs::directory_iterator(nativeRoot / "shader_bin", e))
            {
                const auto name = files::Utf8(entry.path().filename());
                if (StartsWith(name, "ps_") && name.size() > 4 && name.substr(name.size() - 4) == ".cso")
                    programs.push_back(entry.path());
            }
            std::sort(programs.begin(), programs.end(), [](const fs::path& a, const fs::path& b)
                      {
                          auto x = files::Utf8(fs::path(a).make_preferred()), y = files::Utf8(fs::path(b).make_preferred());
                          std::transform(x.begin(), x.end(), x.begin(), ::tolower);
                          std::transform(y.begin(), y.end(), y.begin(), ::tolower);
                          return x < y;
                      });
            for (const auto& path : programs)
            {
                std::string bytes;
                files::ReadBytes(path, bytes);
                if (!Contains(bytes, "floatZSampler"))
                    continue;
                const auto& assembly = AssemblyOf(path);
                const auto& fields = Buffers(assembly);
                const auto near = fields.find("zNear");
                if (near == fields.end() || near->second.slot != 0 || near->second.offset % 16)
                    continue;
                if (Search("div (r\\d+\\.[xyzw]), cb0\\[" + std::to_string(near->second.offset / 16) + "\\]\\.x, \\1\\b", assembly))
                {
                    const std::lock_guard guard(lock);
                    cache[key] = near->second;
                    return near->second;
                }
            }
            throw ShaderError("stock reciprocal-depth reference absent");
        }

        int FreeTextureSlot(const Json& nativePass, const std::string& nativePs)
        {
            std::set<long long> used{13, 15, 14};
            for (const auto& a : nativePass["args"])
                if (I(a["type"]) == 2 || I(a["type"]) == 4)
                    used.insert(I(a["location"]) & 255), used.insert(I(a["location"]) >> 8);
            for (const auto& [slot, _] : ResourceNames(nativePs))
                used.insert(slot);
            static const pyre::Regex sampler("^dcl_sampler s(\\d+),", pyre::MULTILINE);
            for (const auto& m : sampler.FindAll(nativePs))
                used.insert(std::stoll(m[0]));
            for (int i = 0; i < 16; i++)
                if (!used.count(i))
                    return i;
            throw ShaderError("no free texture slot");
        }

        int AddCodeTexture(Json& nativePass, const int code, const std::string& nativePs)
        {
            for (const auto& a : nativePass["args"])
                if (I(a["type"]) == 4 && I(a["u"]["value"]) == code)
                    return static_cast<int>(I(a["location"]) & 255);
            const int slot = FreeTextureSlot(nativePass, nativePs);
            const auto start = static_cast<size_t>(I(nativePass["perPrimArgCount"]) + I(nativePass["perObjArgCount"]));
            auto& args = nativePass["args"];
            size_t position = args.size();
            for (size_t i = start; i < args.size(); i++)
                if (I(args[i]["type"]) != 2 && I(args[i]["type"]) != 4)
                {
                    position = i;
                    break;
                }
            args.insert(args.begin() + static_cast<long long>(position), Json{{"buffer", 0}, {"location", slot | (slot << 8)}, {"size", 1}, {"type", 4}, {"u", Json{{"value", code}}}});
            nativePass["stableArgCount"] = I(nativePass["stableArgCount"]) + 1;
            return slot;
        }

        long long TextureHash(const materials::T6Texture& t)
        {
            return t.name && !t.name->empty() ? T6Hash(*t.name) : (t.nameHash ? static_cast<long long>(*t.nameHash) : -1);
        }

        // add_material_texture: a source texture the donor pass has no slot for
        std::optional<Json> AddMaterialTexture(Json& nativePass, materials::T6Material& outputMaterial, const materials::WawMaterial& sourceMaterial, const long long hash, const std::string& nativePs)
        {
            const auto named = [](const materials::WawTexture& t) { return t.name && !t.name->empty(); };
            const materials::WawTexture* texture = nullptr;
            for (const auto& t : sourceMaterial.textures)
                if (!t.image.empty() && (named(t) ? WawHash(*t.name) : t.nameHash ? static_cast<long long>(*t.nameHash) : -1) == hash)
                {
                    texture = &t;
                    break;
                }
            if (!texture)
                return std::nullopt;
            if (named(*texture) && T6Hash(*texture->name) != hash)
                return std::nullopt;
            auto& textures = outputMaterial.textures;
            if (std::none_of(textures.begin(), textures.end(), [&](const materials::T6Texture& t) { return TextureHash(t) == hash; }))
            {
                textures.push_back(materials::ToT6(*texture));
                std::stable_sort(textures.begin(), textures.end(), [](const auto& a, const auto& b) { return TextureHash(a) < TextureHash(b); });
            }
            const int slot = FreeTextureSlot(nativePass, nativePs);
            Json arg{{"buffer", 0}, {"location", slot | (slot << 8)}, {"size", 1}, {"type", 2}, {"u", Json{{"value", hash}}}};
            const auto start = static_cast<size_t>(I(nativePass["perPrimArgCount"]) + I(nativePass["perObjArgCount"]));
            auto& args = nativePass["args"];
            size_t position = args.size();
            for (size_t i = start; i < args.size(); i++)
                if (I(args[i]["type"]) != 2 || I(args[i]["u"]["value"]) > hash)
                {
                    position = i;
                    break;
                }
            args.insert(args.begin() + static_cast<long long>(position), arg);
            nativePass["stableArgCount"] = I(nativePass["stableArgCount"]) + 1;
            return arg;
        }

        double SoftParticleDepthScale(const std::string& assembly)
        {
            const auto position = pyre::Regex("^\\s*mov (o\\d+), (r\\d+)\\s*$", pyre::MULTILINE).Search(assembly);
            if (!position)
                return 1.0;
            const auto depth = pyre::Regex("^\\s*mul o\\d+\\.zw, " + (*position)[2] + "\\.w, (c\\d+)\\.xyyz\\s*$", pyre::MULTILINE).Search(assembly);
            if (!depth)
                return 1.0;
            const auto literal = pyre::Regex("^\\s*def " + (*depth)[1] + ", ([^\\n]+)", pyre::MULTILINE).Search(assembly);
            if (!literal)
                return 1.0;
            std::vector<double> values;
            const auto listed = (*literal)[1];
            for (const auto part : py::Split(listed, ","))
            {
                double v = 0;
                if (!py::ParseFloat(py::Strip(part), v))
                    throw std::runtime_error("could not convert string to float");
                values.push_back(v);
            }
            return values.size() == 4 && values[1] == 1.0 && values[2] == 0.0 && values[3] == 0.0 ? values[0] : 1.0;
        }

        std::optional<std::pair<std::string, char>> FogOutput(const std::string& sourceAssembly, const Json& contract)
        {
            std::set<std::string> fogRegs;
            for (const auto& [reg, r] : Registers(sourceAssembly).items)
                if (r.name == "fogConsts")
                    fogRegs.insert(reg);
            if (fogRegs.empty())
                return std::nullopt;
            const auto hits = TaintedOutputs(sourceAssembly, fogRegs);
            if (hits.size() != 1)
            {
                std::string text = "[";
                size_t i = 0;
                for (const auto& [reg, c] : hits)
                    text += (i++ ? ", " : "") + std::string("(") + gsc::Repr(reg) + ", " + gsc::Repr(std::string(1, c)) + ")";
                throw ShaderError("unrecognized vertex fog output " + text + "]");
            }
            const auto [reg, comp] = *hits.begin();
            if (!contract["outputs"].contains(reg))
                throw ShaderError("vertex fog output is not declared");
            return std::pair{reg, comp};
        }

        std::map<std::string, std::pair<int, std::string>> CompiledSignature(const std::string& code, const std::string& kind)
        {
            const auto text = Disassemble(code);
            const auto match = pyre::Regex("// " + kind + " signature:\\s*\\n//\\s*\\n// Name.*?\\n// -.*?\\n(.*?)//\\s*\\n", pyre::DOTALL).Search(text);
            std::map<std::string, std::pair<int, std::string>> out;
            if (!match)
                return out;
            static const pyre::Regex row("^//\\s+(\\w+)\\s+(\\d+)\\s+([xyzw]+)\\s+(\\d+)", pyre::MULTILINE);
            for (const auto& m : row.FindAll((*match)[1]))
                out[Upper(m[0]) + "\x1f" + m[1]] = {std::stoi(m[3]), m[2]};
            return out;
        }
    } // namespace

    uint32_t WawHash(const std::string& name)
    {
        uint32_t value = 0;
        for (const unsigned char c : name)
            value = (value * 33) ^ static_cast<unsigned char>(std::tolower(c));
        return value;
    }

    uint32_t T6Hash(const std::string& name)
    {
        uint32_t value = 0;
        for (const unsigned char c : name)
            value = (value * 33) ^ (c | 0x20u);
        return value;
    }

    bool IsScorchProgram(const materials::WawMaterial& sourceMaterial)
    {
        return Contains(sourceMaterial.techniqueSet, "_sco");
    }

    const std::string& NativeProgram(const fs::path& root, const std::string& stage, const Json& pass)
    {
        const auto& shader = pass[stage == "vs" ? "vertexShader" : "pixelShader"];
        const auto name = shader.contains("name") && shader["name"].is_string() ? shader["name"].get<std::string>() : std::string();
        if (name.empty())
            throw ShaderError("native pass has no " + stage + " program");
        return AssemblyOf(root / "shader_bin" / files::FromUtf8(stage + "_" + name + ".cso"));
    }

    std::string EmbedLiterals(std::string assembly, const Json& sourcePass, const std::string& stage)
    {
        const int kind = stage == "ps" ? 7 : 1;
        std::string definitions;
        bool first = true;
        for (const auto& a : sourcePass["args"])
        {
            if (I(a["type"]) != kind)
                continue;
            std::string line = "def c" + std::to_string(I(a["dest"])) + ", ";
            size_t i = 0;
            for (const auto& v : a["literal"])
                line += (i++ ? ", " : "") + F(v.get<double>());
            definitions += (first ? "" : "\n") + line;
            first = false;
        }
        const auto profile = stage + "_3_0";
        if (const auto at = assembly.find(profile); at != std::string::npos)
            assembly.replace(at, profile.size(), profile + "\n" + definitions);
        return assembly;
    }

    Json PixelContract(const std::string& sourceAssembly, const Json& sourcePass, const Json& nativePass, const fs::path& nativeRoot, const materials::WawMaterial& sourceMaterial)
    {
        const auto ir = Ir(sourceAssembly);
        const auto& vs = NativeProgram(nativeRoot, "vs", nativePass);
        const auto& ps = NativeProgram(nativeRoot, "ps", nativePass);
        const auto available = Signature(vs, "Output");
        const auto targets = Signature(ps, "Output");
        if (targets.size() != 1 || !targets.count("SV_TARGET0"))
            throw ShaderError("native pass uses a different render-target contract");
        Json contract{{"inputs", Json::object()}, {"outputs", Json::object()}, {"constants", Json::object()}, {"samplers", Json::object()}};
        for (const auto& [reg, decl] : ir["inputs"].items())
        {
            const auto semantic = decl[0].get<std::string>();
            const int width = decl[1].get<int>();
            const auto key = Upper(semantic);
            if ((key != "COLOR0" && key != "TEXCOORD0") || (key == "TEXCOORD0" && width > 3))
                throw ShaderError("vertex adapter required for " + semantic + "/" + std::to_string(width));
            const auto it = available.find(key);
            if (it == available.end() || it->second.first < width || it->second.second != "float")
                throw ShaderError("native vertex output cannot supply " + semantic + "/" + std::to_string(width));
            if (key == "TEXCOORD0" && width == 3 && !Buffers(vs).count("fogConsts"))
                throw ShaderError("native vertex program has no verified fog varying");
            contract["inputs"][reg] = Json{{"semantic", semantic}, {"width", width}};
        }
        for (const auto& [reg, decl] : ir["outputs"].items())
        {
            const auto semantic = decl[0].get<std::string>();
            const int width = decl[1].get<int>();
            if (Upper(semantic) != "SV_TARGET0" || width != 4)
                throw ShaderError("unsupported source target " + semantic);
            contract["outputs"][reg] = Json{{"semantic", semantic}, {"width", width}};
        }
        const auto& names = Registers(sourceAssembly);
        auto nativeFields = Buffers(vs);
        for (const auto& [k, v] : Buffers(ps))
            nativeFields[k] = v;
        const auto sourceArgs = ConstantArgs(sourcePass);
        for (const auto& reg : Keys(ir["constants"]))
        {
            const auto it = sourceArgs.find(reg);
            if (it == sourceArgs.end())
                throw ShaderError("no original pass argument for " + reg);
            const auto& arg = it->second;
            const auto type = I(arg["type"]);
            if (type == 5)
            {
                const auto* named = names.Find(reg);
                if (!named || !nativeFields.count(named->name))
                    throw ShaderError("no native code uniform for " + reg + ": " + RegisterRepr(named));
                const long long row = (arg.contains("firstRow") ? I(arg["firstRow"]) : 0) + std::stoll(reg.substr(1)) - I(arg["dest"]);
                const auto field = nativeFields[named->name];
                if (field.slot != 0 || field.offset % 16 || field.size < 16 * (row + 1))
                    throw ShaderError("code uniform requires an adapter: " + named->name);
                contract["constants"][reg] = Json{{"buffer", field.slot}, {"index", field.offset / 16 + row}};
            }
            else if (type == 6)
            {
                const auto* found = FindArg(nativePass, 6, I(arg["value"]));
                if (!found || I((*found)["location"]) % 16 || I((*found)["size"]) < 16)
                {
                    // the donor has no slot: embed this material's own WaW value
                    const auto literal = MaterialLiteral(sourceMaterial, I(arg["value"]));
                    if (!literal)
                        throw ShaderError("native material constant binding absent for " + reg);
                    contract["constants"][reg] = Json{{"expr", Float4(*literal)}, {"uses", Json::array()}};
                    AddTo(contract, "embedded_material_constants", arg["value"]);
                    continue;
                }
                contract["constants"][reg] = Json{{"buffer", (*found)["buffer"]}, {"index", I((*found)["location"]) / 16}};
            }
            else
                throw ShaderError("original literal argument needs source embedding: " + reg);
        }
        std::map<std::string, Json> textureArgs;
        for (const auto& a : sourcePass["args"])
            if (I(a["type"]) == 2 || I(a["type"]) == 4)
                textureArgs["s" + std::to_string(I(a["dest"]))] = a;
        for (const auto& [reg, dimJson] : ir["samplers"].items())
        {
            const auto dim = dimJson.get<std::string>();
            const auto it = textureArgs.find(reg);
            if (it == textureArgs.end() || I(it->second["type"]) != 2)
                throw ShaderError("code texture adapter required for " + reg);
            const auto* found = FindArg(nativePass, 2, I(it->second["value"]));
            if (!found || dim != "2d")
                throw ShaderError("native material texture binding absent for " + reg + "/" + dim);
            contract["samplers"][reg] = Json{{"texture", I((*found)["location"]) & 255}, {"sampler", I((*found)["location"]) >> 8}};
        }
        return contract;
    }

    Json PairedPixelContract(const std::string& sourceAssembly, const Json& sourcePass, Json& nativePass, const fs::path& nativeRoot, const Json& linked,
                             const std::optional<std::string>& visibility, const materials::WawMaterial& sourceMaterial, const std::optional<std::array<double, 4>>& falloffPlacement,
                             materials::T6Material* outputMaterial)
    {
        const auto ir = Ir(sourceAssembly);
        const auto& vs = NativeProgram(nativeRoot, "vs", nativePass);
        const auto& ps = NativeProgram(nativeRoot, "ps", nativePass);
        const auto targets = Signature(ps, "Output");
        if (targets.size() != 1 || !targets.count("SV_TARGET0"))
            throw ShaderError("native pass uses a different render-target contract");
        std::map<std::string, int> provided;
        for (const auto& s : linked)
            provided[Upper(s["semantic"].get<std::string>())] = s["width"].get<int>();
        Json contract{{"inputs", Json::object()}, {"outputs", Json::object()}, {"constants", Json::object()}, {"samplers", Json::object()}, {"input_signature", linked}};
        for (const auto& [reg, decl] : ir["inputs"].items())
        {
            const auto semantic = decl[0].get<std::string>();
            const int width = decl[1].get<int>();
            const auto p = provided.find(Upper(semantic));
            if ((p == provided.end() ? 0 : p->second) < width)
                throw ShaderError("translated vertex program does not supply " + semantic + "/" + std::to_string(width));
            contract["inputs"][reg] = Json{{"semantic", semantic}, {"width", width}};
        }
        for (const auto& [reg, decl] : ir["outputs"].items())
        {
            const auto semantic = decl[0].get<std::string>();
            const int width = decl[1].get<int>();
            if (Upper(semantic) != "SV_TARGET0" || width != 4)
                throw ShaderError("unsupported source target " + semantic);
            contract["outputs"][reg] = Json{{"semantic", semantic}, {"width", width}};
        }
        const auto& names = Registers(sourceAssembly);
        // WaW packs the local light's ramp into its secondary lightmap; T6 binds
        // the source light def's ramp per light instead
        std::optional<std::string> ramp;
        for (const auto& [r, n] : names.items)
            if (n.name == "attenuationSampler")
            {
                ramp = r;
                break;
            }
        auto nativeFields = Buffers(vs);
        for (const auto& [k, v] : Buffers(ps))
            nativeFields[k] = v;
        const bool sqrtHdr = NativeSqrtHdrOutput(ps);
        const auto hdr = nativeFields.find("hdrControl0");
        if (sqrtHdr && (hdr == nativeFields.end() || hdr->second.slot != 0 || hdr->second.offset % 16))
            throw ShaderError("native hdr scale not addressable");
        const std::optional<int> hdrRow = sqrtHdr ? std::optional<int>(hdr->second.offset / 16) : std::nullopt;
        const auto sourceArgs = ConstantArgs(sourcePass);
        for (const auto& reg : Keys(ir["constants"]))
        {
            const auto it = sourceArgs.find(reg);
            if (it == sourceArgs.end())
                throw ShaderError("no original pass argument for " + reg);
            const auto& arg = it->second;
            const auto type = I(arg["type"]);
            if (type == 6)
            {
                const auto* found = FindArg(nativePass, 6, I(arg["value"]));
                if (!found || I((*found)["location"]) % 16 || I((*found)["size"]) < 16)
                {
                    const auto literal = MaterialLiteral(sourceMaterial, I(arg["value"]));
                    if (!literal)
                        throw ShaderError("native material constant binding absent for " + reg);
                    contract["constants"][reg] = Json{{"expr", Float4(*literal)}, {"uses", Json::array()}};
                    AddTo(contract, "embedded_material_constants", arg["value"]);
                    continue;
                }
                contract["constants"][reg] = Json{{"buffer", (*found)["buffer"]}, {"index", I((*found)["location"]) / 16}};
                continue;
            }
            if (type != 5)
                throw ShaderError("original literal argument needs source embedding: " + reg);
            const auto* named = names.Find(reg);
            std::string name = named ? named->name : std::string();
            const bool hasName = named != nullptr;
            if (ramp && name == "lightFalloffPlacement")
            {
                contract["constants"][reg] = Json{{"expr", "float4(1, 0, 0, 0.5)"}, {"uses", Json::array()}};
                AddConversion(contract, "local light ramp: source light def bound per light");
                continue;
            }
            if (name == "gameTime" && !nativeFields.count(name))
            {
                // T6 CONST_SRC_CODE_GAMETIME (0x19): stock pixel passes route it to buffer 2 row 4
                const Json binding{{"type", 5}, {"buffer", 2}, {"location", 64}, {"size", 16}, {"u", Json{{"value", 0x01000019}}}};
                const auto& args = nativePass["args"];
                if (std::none_of(args.begin(), args.end(), [&](const Json& a) { return py::SortKeys(a) == py::SortKeys(binding); }))
                {
                    if (std::any_of(args.begin(), args.end(), [](const Json& a) { return I(a["type"]) == 5 && I(a["buffer"]) == 2 && I(a["location"]) == 64; }))
                        throw ShaderError("pixel game time row is taken");
                    nativePass["args"].push_back(binding);
                    nativePass["stableArgCount"] = I(nativePass["stableArgCount"]) + 1;
                }
                contract["constants"][reg] = Json{{"buffer", 2}, {"index", 4}};
                AddConversion(contract, "gameTime: T6 code constant 0x19");
                continue;
            }
            if (hasName && LIGHT_ADAPTERS.count(name))
            {
                contract["constants"][reg] = LightConstant(name, nativeFields, sqrtHdr ? hdrRow : std::nullopt, falloffPlacement, &nativePass);
                AddConversion(contract, LIGHT_ADAPTERS.at(name));
                continue;
            }
            if (!nativeFields.count(name) && CODE_UNIFORM_ALIASES.count(name))
                name = CODE_UNIFORM_ALIASES.at(name);
            if (!hasName || !nativeFields.count(name))
                throw ShaderError("no native code uniform for " + reg + ": " + RegisterRepr(named));
            const long long row = (arg.contains("firstRow") ? I(arg["firstRow"]) : 0) + std::stoll(reg.substr(1)) - I(arg["dest"]);
            const auto field = nativeFields[name];
            if (field.slot != 0 || field.offset % 16 || field.size < 16 * (row + 1))
                throw ShaderError("code uniform requires an adapter: " + name);
            const long long index = field.offset / 16 + row;
            if ((name == "fogColor" || name == "sunDiffuse") && sqrtHdr)
            {
                // T6 keeps these linear; WaW's programs sum them in gamma space
                contract["constants"][reg] = Json{{"expr", "float4(sqrt(max(t6_cb0[" + std::to_string(index) + "].xyz * " + HdrDisplay(*hdrRow) + ", 0)), t6_cb0[" +
                                                               std::to_string(index) + "].w)"},
                                                  {"uses", Json::array({Json::array({0, index}), Json::array({0, *hdrRow})})}};
                AddConversion(contract, name + ": sqrt(4 hdr * linear)");
            }
            else
                contract["constants"][reg] = Json{{"buffer", field.slot}, {"index", index}};
        }
        auto nativeTextures = ResourceNames(ps);
        const auto textureName = [&](const int slot) -> const Resource*
        {
            for (const auto& [s, r] : nativeTextures)
                if (s == slot)
                    return &r;
            return nullptr;
        };
        std::map<std::string, Json> sourceSamplers;
        for (const auto& a : sourcePass["args"])
            if (I(a["type"]) == 2 || I(a["type"]) == 4)
                sourceSamplers["s" + std::to_string(I(a["dest"]))] = a;
        const auto hasSampler = [&](const int slot, const std::string& suffix) { return Search("^dcl_sampler s" + std::to_string(slot) + suffix, ps, pyre::MULTILINE); };
        for (const auto& [reg, dimJson] : ir["samplers"].items())
        {
            const auto dim = dimJson.get<std::string>();
            std::optional<Json> arg;
            if (const auto it = sourceSamplers.find(reg); it != sourceSamplers.end())
                arg = it->second;
            const auto* named = names.Find(reg);
            std::optional<std::string> sourceName = named ? std::optional<std::string>(named->name) : std::nullopt;
            if (ramp && reg == *ramp)
                sourceName = "attenuationSampler";
            if (!arg && !(sourceName && PER_SURFACE_SAMPLERS.count(*sourceName)))
                throw ShaderError("no original sampler argument for " + reg);
            const auto sname = sourceName.value_or(std::string());
            long long texture = 0, sampler = 0;
            bool code = true;
            if (arg && I((*arg)["type"]) == 2)
            {
                code = false;
                const Json* found = FindArg(nativePass, 2, I((*arg)["value"]));
                std::optional<Json> added;
                if (!found && outputMaterial)
                {
                    added = AddMaterialTexture(nativePass, *outputMaterial, sourceMaterial, I((*arg)["value"]), ps);
                    if (added)
                        found = &*added;
                }
                if (!found)
                    throw ShaderError("native texture binding absent for " + reg + " (" + (sourceName ? *sourceName : "None") + ")");
                texture = I((*found)["location"]) & 255, sampler = I((*found)["location"]) >> 8;
                if (added)
                {
                    const auto it = std::find_if(nativeTextures.begin(), nativeTextures.end(), [&](const auto& e) { return e.first == texture; });
                    if (it != nativeTextures.end())
                        it->second = Resource{sname, "2d"};
                    else
                        nativeTextures.emplace_back(static_cast<int>(texture), Resource{sname, "2d"});
                }
            }
            else
            {
                // code textures: identified by their reflected name in both programs
                if (StartsWith(sname, "terrainScorchTextureSampler") && IsScorchProgram(sourceMaterial))
                {
                    contract["samplers"][reg] = Json{{"texture", 0}, {"sampler", 0}, {"constant", Json::array({SCORCH_NEUTRAL[0], SCORCH_NEUTRAL[1], SCORCH_NEUTRAL[2], SCORCH_NEUTRAL[3]})}};
                    AddConversion(contract, sname + ": unscorched (weight 0)");
                    continue;
                }
                const auto lookup = WAW_PAGE_UV.count(sname) ? std::string("lightmapSamplerSecondary") : sname;
                std::vector<int> slots;
                for (const auto& [t, r] : nativeTextures)
                    if (r.name == lookup && sourceName)
                        slots.push_back(t);
                if (slots.empty() && sourceName && CODE_TEXTURE_ARGS.count(lookup))
                {
                    slots = {AddCodeTexture(nativePass, CODE_TEXTURE_ARGS.at(lookup), ps)};
                    const auto it = std::find_if(nativeTextures.begin(), nativeTextures.end(), [&](const auto& e) { return e.first == slots[0]; });
                    if (it != nativeTextures.end())
                        it->second = Resource{lookup, "2d"};
                    else
                        nativeTextures.emplace_back(slots[0], Resource{lookup, "2d"});
                }
                else if (slots.empty() && sourceName && WORLD_SURFACE_TEXTURE_SLOTS.count(lookup) && sqrtHdr && nativeFields.count("hdrControl0"))
                {
                    const auto& [slot, d] = WORLD_SURFACE_TEXTURE_SLOTS.at(lookup);
                    slots = {slot};
                    const auto it = std::find_if(nativeTextures.begin(), nativeTextures.end(), [&](const auto& e) { return e.first == slot; });
                    if (it != nativeTextures.end())
                        it->second = Resource{lookup, d};
                    else
                        nativeTextures.emplace_back(slot, Resource{lookup, d});
                }
                else if (slots.size() != 1 || !hasSampler(slots[0], ","))
                    throw ShaderError("native code texture absent for " + reg + " (" + (sourceName ? *sourceName : "None") + ")");
                texture = sampler = slots[0];
                arg = Json{{"type", 4}};
            }
            const auto* native = textureName(static_cast<int>(texture));
            const bool sourceSky = I((*arg)["type"]) == 2 && StartsWith(sourceMaterial.techniqueSet, "mc_sky");
            const std::string mapped = dim == "2d" ? "2d" : dim == "volume" ? "3d" : dim == "cube" ? "cube" : throw std::runtime_error("KeyError: " + gsc::Repr(dim));
            if (!sourceSky && (!native || mapped != native->dim))
                throw ShaderError("texture dimension differs for " + reg + ": " + dim + " vs " + (native ? native->dim : "None"));
            Json entry{{"texture", texture}, {"sampler", sampler}};
            if (I((*arg)["type"]) == 4)
            {
                if (sname == "shadowmapSamplerSun" || sname == "shadowmapSamplerSpot")
                {
                    if (!hasSampler(static_cast<int>(sampler), ", mode_comparison"))
                        throw ShaderError("native " + sname + " sampler mode not recognized");
                    entry["point_load"] = true;
                    entry["reverse_depth_compare"] = true;
                    AddConversion(contract, sname + ": texel load at mip 0");
                }
                else if (sname == "floatZSampler")
                {
                    std::optional<Field> nearField;
                    if (const auto n = nativeFields.find("zNear"); n != nativeFields.end())
                        nearField = n->second;
                    if (!nearField || !Search("div (r\\d+\\.[xyzw]), cb0\\[" + std::to_string(nearField->offset / 16) + "\\]\\.x, \\1\\b", ps))
                    {
                        const auto reference = NativeDepthReference(nativeRoot);
                        if (nearField && !(*nearField == reference))
                            throw ShaderError("donor/reference depth uniform layouts differ");
                        nearField = reference;
                    }
                    if (nearField->slot != 0 || nearField->offset % 16)
                        throw ShaderError("native reciprocal-depth scale absent");
                    const int nearRow = nearField->offset / 16;
                    entry["texel_expr"] = "float4(t6_cb0[" + std::to_string(nearRow) + "].x / max(abs(T.x), 1e-20), T.yzw)";
                    entry["uses"] = Json::array({Json::array({0, nearRow})});
                    AddConversion(contract, "floatZ: reciprocal to camera depth");
                }
                else if (sname == "refractSampler")
                    AddConversion(contract, "refractSampler: native scene color code texture 9");
                else if (sname == "modelLightingSampler" && sqrtHdr)
                {
                    if (!Search("(?:mul|mad) r\\d+\\.\\w+, r\\d+\\.\\w+, l\\((?:[^)]*32\\.000000){3}[^)]*\\)", ps))
                        throw ShaderError("native model lighting scale not recognized");
                    entry["rgb_scale"] = "sqrt(8.0 * " + HdrDisplay(*hdrRow) + ")";
                    entry["uses"] = Json::array({Json::array({0, *hdrRow})});
                    AddConversion(contract, "modelLighting: sqrt(8 * 4 hdr) * L");
                    bool lights = false;
                    for (const auto& [_, n] : names.items)
                        lights |= n.name == "sunDiffuse" || n.name == "lightDiffuse";
                    if (lights)
                    {
                        std::optional<size_t> link;
                        for (size_t i = 0; i < linked.size() && !link; i++)
                            if (visibility && Upper(linked[i]["semantic"].get<std::string>()) == Upper(*visibility) && linked[i]["width"] == 4)
                                link = i;
                        if (!link)
                            throw ShaderError("primary light visibility varying absent");
                        entry["alpha_expr"] = "input.link" + std::to_string(*link) + ".w";
                    }
                }
                else if (sname == "attenuationSampler")
                {
                }
                else if (WAW_PAGE_UV.count(sname))
                {
                    const auto& [scale, offset] = WAW_PAGE_UV.at(sname);
                    entry["v_scale_offset"] = Json::array({scale, offset});
                    entry["u_scale_offset"] = Json::array({0.5, 0.5});
                    contract["lightmap"] = "waw";
                }
                else if (sname == "reflectionProbeSampler" && sqrtHdr)
                    AddConversion(contract, "reflectionProbe: source RGBA, multiply after filtering");
                else
                    throw ShaderError("code texture " + (sourceName ? *sourceName : "None") + " needs a unit adapter");
            }
            contract["samplers"][reg] = entry;
            if (code)
            {
                const auto bitName = WAW_PAGE_UV.count(sname) ? std::string("lightmapSamplerSecondary") : sname;
                if (const auto bit = CUSTOM_SAMPLER_BITS.find(bitName); bit != CUSTOM_SAMPLER_BITS.end())
                    nativePass["customSamplerFlags"] = (nativePass.contains("customSamplerFlags") ? I(nativePass["customSamplerFlags"]) : 0) | bit->second;
            }
        }
        return contract;
    }

    std::pair<std::string, Json> BindLocalLightRamp(std::string assembly, const Json& sourcePass, const materials::WawMaterial& sourceMaterial)
    {
        // local_light_ramp: the sole secondary-lightmap read is a placed 1D lookup
        const auto& names = Registers(assembly);
        std::optional<std::string> placement, sampler;
        for (const auto& [r, n] : names.items)
        {
            if (!placement && n.name == "lightFalloffPlacement")
                placement = r;
            if (!sampler && n.name == "lightmapSamplerSecondary")
                sampler = r;
        }
        if (!placement || !sampler)
            return {assembly, sourcePass};
        const auto probe = "\\bmad(?:_pp)?\\s+(r\\d+)\\.xy,\\s*r\\d+\\.[xyzw],\\s*" + Escape(*placement) + ",\\s*" + Escape(*placement) +
                           "\\.zwzw\\s*\\n\\s*texld(?:_pp)?\\s+r\\d+,\\s*\\1,\\s*" + Escape(*sampler) + "\\b";
        if (!Search(probe, assembly))
            return {assembly, sourcePass};
        auto adapted = sourcePass;
        std::optional<std::string> unused;
        for (int i = 0; i < 16 && !unused; i++)
            if (!names.Find("s" + std::to_string(i)))
                unused = "s" + std::to_string(i);
        if (!unused && IsScorchProgram(sourceMaterial))
        {
            // all scorch samplers return the same unscorched zero in T6: merge aliases
            std::vector<std::string> scorch;
            for (const auto& [r, n] : names.items)
                if (StartsWith(n.name, "terrainScorchTextureSampler"))
                    scorch.push_back(r);
            if (scorch.size() > 1)
            {
                unused = scorch[1];
                std::set<long long> removed;
                for (size_t i = 1; i < scorch.size(); i++)
                {
                    assembly = pyre::Regex("^\\s*dcl_2d " + Escape(scorch[i]) + "\\s*$", pyre::MULTILINE).Sub("", assembly);
                    assembly = pyre::Regex("\\b" + Escape(scorch[i]) + "\\b").Sub([&](const pyre::Match&) { return scorch[0]; }, assembly);
                    removed.insert(std::stoll(scorch[i].substr(1)));
                }
                Json kept = Json::array();
                for (const auto& a : adapted["args"])
                    if (!(I(a["type"]) == 4 && removed.count(I(a["dest"]))))
                        kept.push_back(a);
                adapted["args"] = kept;
            }
        }
        if (!unused)
            return {assembly, sourcePass};
        const auto pattern = "(\\bmad(?:_pp)?\\s+(r\\d+)\\.xy,\\s*r\\d+\\.[xyzw],\\s*" + Escape(*placement) + ",\\s*" + Escape(*placement) +
                             "\\.zwzw\\s*\\n\\s*texld(?:_pp)?\\s+r\\d+,\\s*\\2,\\s*)" + Escape(*sampler) + "\\b";
        size_t count = 0;
        assembly = pyre::Regex(pattern).Sub([&](const pyre::Match& m) { return m[1] + *unused; }, assembly, 0, &count);
        if (count != 1)
            throw ShaderError("ambiguous local light ramp lookup");
        assembly = "// attenuationSampler " + *unused + " 1\n" + assembly;
        if (const auto at = assembly.find("ps_3_0"); at != std::string::npos)
            assembly.replace(at, 6, "ps_3_0\n    dcl_2d " + *unused);
        adapted["args"].push_back(Json{{"type", 4}, {"dest", std::stoll(unused->substr(1))}, {"value", 0}});
        return {assembly, adapted};
    }

    std::pair<std::string, bool> NormalizeSoftParticleDepth(const std::string& assembly, const std::string& pixelAssembly)
    {
        const double scale = SoftParticleDepthScale(assembly);
        bool reads = false;
        for (const auto& [_, n] : Registers(pixelAssembly).items)
            reads |= n.name == "floatZSampler";
        if (scale == 1.0 || !reads)
            return {assembly, false};
        size_t count = 0;
        auto adapted = pyre::Regex("(^\\s*)mul (o\\d+)\\.zw, (r\\d+)\\.w, c\\d+\\.xyyz\\s*$", pyre::MULTILINE).Sub("\\1mov \\2.zw, \\3.w", assembly, 0, &count);
        if (count != 1)
            throw ShaderError("ambiguous packed soft-particle depth varying");
        return {adapted, true};
    }

    Json VertexContract(const std::string& sourceAssembly, const Json& sourcePass, Json& nativePass, const fs::path& nativeRoot, const bool paired,
                        const materials::WawMaterial& sourceMaterial, const bool layered, const bool trailGeometry)
    {
        const auto ir = Ir(sourceAssembly);
        const auto& nativeVs = NativeProgram(nativeRoot, "vs", nativePass);
        auto incoming = Signature(nativeVs, "Input");
        const auto techniqueSet = sourceMaterial.techniqueSet;
        const bool sky = StartsWith(techniqueSet, "mc_sky");
        if (paired && sky)
        {
            incoming.try_emplace("COLOR0", 4, "float");
            incoming.try_emplace("TEXCOORD0", 2, "float");
        }
        const bool falloffNormal = paired && materials::ParseTechnique(techniqueSet).unlit == "unlit_falloff_add";
        if (falloffNormal)
            incoming.try_emplace("NORMAL0", 3, "float");
        const auto& fields = Buffers(nativeVs);
        const auto& names = Registers(sourceAssembly);
        Json contract{{"inputs", Json::object()}, {"outputs", Json::object()}, {"constants", Json::object()}, {"samplers", Json::object()}};
        if (paired && sky)
        {
            // source sky fog uses a normalized direction at a shader-defined distance
            const auto match = pyre::Regex("nrm (r\\d+)\\.xyz, v\\d+\\s*\\n\\s*mul \\1\\.xyz, \\1, (c\\d+)\\.([xyzw])").Search(sourceAssembly);
            if (match)
                if (const auto literal = pyre::Regex("def " + (*match)[2] + ", ([^\\n]+)").Search(sourceAssembly))
                {
                    const auto parts = py::Split((*literal)[1], ",");
                    double v = 0;
                    py::ParseFloat(py::Strip(parts.at(std::string("xyzw").find((*match)[3]))), v);
                    contract["sky_fog_distance"] = v;
                }
        }
        const auto readsOf = [&](const std::string& reg, const std::vector<std::string>& skip)
        {
            std::vector<std::string> reads;
            const pyre::Regex word("\\b" + Escape(reg) + "\\b");
            for (const auto raw : py::SplitLines(sourceAssembly))
            {
                const auto line = std::string(py::Strip(raw));
                if (std::any_of(skip.begin(), skip.end(), [&](const std::string& s) { return StartsWith(line, s); }))
                    continue;
                if (word.Search(std::string(raw)))
                    reads.push_back(line);
            }
            return reads;
        };
        for (const auto& [reg, decl] : ir["inputs"].items())
        {
            const auto semantic = decl[0].get<std::string>();
            const int width = decl[1].get<int>();
            const auto key = Upper(semantic);
            std::vector<std::string> allowed{"POSITION0", "COLOR0", "TEXCOORD0"};
            if (paired)
                allowed.insert(allowed.end(), PACKED_VECTOR_INPUTS.begin(), PACKED_VECTOR_INPUTS.end());
            if (paired && layered)
                allowed.insert(allowed.end(), LAYER_STREAM_INPUTS.begin(), LAYER_STREAM_INPUTS.end());
            if (key == "BLENDWEIGHT0" && paired && IsScorchProgram(sourceMaterial))
            {
                contract["inputs"][reg] = Json{{"semantic", semantic}, {"width", width}, {"adapter", "waw_neutral"},
                                               {"constant", Json::array({SCORCH_NEUTRAL[0], SCORCH_NEUTRAL[1], SCORCH_NEUTRAL[2], SCORCH_NEUTRAL[3]})}};
                AddTo(contract, "neutral", "terrain scorch weights: unscorched (0)");
                continue;
            }
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end() || !incoming.count(key))
                throw ShaderError("packed vertex adapter required for " + semantic);
            const auto [nativeWidth, typ] = incoming[key];
            if (typ != "float")
                throw ShaderError("unsupported native attribute type " + semantic + "/" + typ);
            Json entry{{"semantic", semantic}, {"width", width}};
            if (std::find(PACKED_VECTOR_INPUTS.begin(), PACKED_VECTOR_INPUTS.end(), key) != PACKED_VECTOR_INPUTS.end())
            {
                if (width != 4 || nativeWidth != 3 || !Contains(sourceAssembly, "0.00787401572") || !Contains(sourceAssembly, "0.752941191"))
                    throw ShaderError("unrecognized packed vector encoding for " + semantic);
                entry["width"] = 3, entry["adapter"] = "waw_ubyte4_vector";
            }
            else if (key == "TEXCOORD0" && width == 4 && nativeWidth >= 2)
            {
                if (Contains(sourceAssembly, "0.0009765625") && Contains(sourceAssembly, "0.0078125") && Contains(sourceAssembly, "3.05175781e-005"))
                    entry["width"] = 2, entry["adapter"] = "waw_half_uv";
                else
                {
                    const auto reads = readsOf(reg, {"//", "dcl_"});
                    const pyre::Regex form("(?:mov\\s+o\\d+\\.xy,\\s*|add\\s+r\\d+\\.xy,\\s*r\\d+(?:\\.xy)?,\\s*-?)" + Escape(reg) + "(?:\\.xy)?");
                    const auto t1 = incoming.find("TEXCOORD1");
                    if (!reads.empty() && std::all_of(reads.begin(), reads.end(), [&](const std::string& l) { return form.FullMatch(l).has_value(); }))
                        entry["width"] = 2, entry["adapter"] = "waw_uv";
                    else if (paired && t1 != incoming.end() && t1->second.first >= 2 && t1->second.second == "float")
                        entry["width"] = 2, entry["adapter"] = "waw_uv_lightmap";
                    else
                        throw ShaderError("unrecognized legacy UV packing");
                }
            }
            else if (key == "POSITION0" && width == 4 && nativeWidth == 3)
                entry["width"] = 3, entry["adapter"] = "waw_position";
            else if (nativeWidth < width)
                throw ShaderError("native input cannot supply " + semantic + "/" + std::to_string(width));
            contract["inputs"][reg] = entry;
        }
        for (const auto& [reg, decl] : ir["outputs"].items())
        {
            const auto semantic = decl[0].get<std::string>();
            const auto key = Upper(semantic);
            if (!paired && key != "SV_POSITION" && key != "COLOR0" && key != "TEXCOORD0")
                throw ShaderError("vertex varying adapter required for " + semantic);
            contract["outputs"][reg] = Json{{"semantic", semantic}, {"width", decl[1]}};
        }
        for (const auto& reg : Keys(ir["constants"]))
        {
            const long long number = std::stoll(reg.substr(1));
            const Json* materialArg = nullptr;
            for (const auto& a : sourcePass["args"])
                if (I(a["type"]) == 0 && I(a["dest"]) == number)
                {
                    materialArg = &a;
                    break;
                }
            const auto* named = names.Find(reg);
            const std::string sourceName = named ? named->name : std::string();
            if (materialArg)
            {
                auto literal = MaterialLiteral(sourceMaterial, I((*materialArg)["value"]));
                if (!literal)
                    throw ShaderError("original vertex material constant absent for " + reg);
                if (trailGeometry && sourceName == "falloffParms")
                {
                    literal = std::vector<double>(std::begin(FACE_ON_FALLOFF_PARMS), std::end(FACE_ON_FALLOFF_PARMS));
                    contract["face_on_falloff"] = true;
                }
                contract["constants"][reg] = Json{{"expr", Float4(*literal)}, {"uses", Json::array()}};
                continue;
            }
            const Json* arg = nullptr;
            for (const auto& a : sourcePass["args"])
                if (I(a["type"]) == 3 && I(a["dest"]) <= number && number < I(a["dest"]) + I(a["rowCount"]))
                {
                    arg = &a;
                    break;
                }
            const auto rowOf = [&] { return (arg->contains("firstRow") ? I((*arg)["firstRow"]) : 0) + number - I((*arg)["dest"]); };
            if (arg && sourceName == "gameTime" && sky)
            {
                const Json binding{{"type", 3}, {"buffer", 2}, {"location", 240}, {"size", 16}, {"u", Json{{"value", 0x01000019}}}};
                const auto& args = nativePass["args"];
                if (std::none_of(args.begin(), args.end(), [&](const Json& a) { return py::SortKeys(a) == py::SortKeys(binding); }))
                {
                    nativePass["args"].push_back(binding);
                    nativePass["stableArgCount"] = (nativePass.contains("stableArgCount") ? I(nativePass["stableArgCount"]) : 0) + 1;
                }
                contract["constants"][reg] = Json{{"buffer", 2}, {"index", 15}};
                continue;
            }
            if (arg && sourceName == "gameTime" && IsScorchProgram(sourceMaterial))
            {
                contract["constants"][reg] = Json{{"expr", "float4(0.0, 0.0, 0.0, 0.0)"}, {"uses", Json::array()}};
                continue;
            }
            if (arg && sourceName == "worldViewProjectionMatrix")
            {
                const auto wm = fields.find("worldMatrix"), vp = fields.find("viewProjectionMatrix");
                if (wm == fields.end() || vp == fields.end() || wm->second.size != 64 || vp->second.size != 64)
                    throw ShaderError("matrix composition inputs absent");
                const auto row = rowOf();
                const auto v = "t6_cb" + std::to_string(vp->second.slot) + "[" + std::to_string(vp->second.offset / 16 + row) + "]";
                std::string expr;
                Json uses = Json::array({Json::array({vp->second.slot, vp->second.offset / 16 + row})});
                for (int i = 0; i < 4; i++)
                {
                    auto w = "t6_cb" + std::to_string(wm->second.slot) + "[" + std::to_string(wm->second.offset / 16 + i) + "]";
                    if (sky)
                        w = "float4(" + w + ".xyz, 0)";
                    expr += (i ? " + " : "") + v + "." + "xyzw"[i] + " * " + w;
                }
                for (int i = 0; i < 4; i++)
                    uses.push_back(Json::array({wm->second.slot, wm->second.offset / 16 + i}));
                contract["constants"][reg] = Json{{"expr", expr}, {"uses", uses}};
                continue;
            }
            if (arg && sourceName == "inverseWorldViewMatrix")
            {
                const auto row = rowOf();
                const auto reads = readsOf(reg, {"//", "def"});
                const auto wm = fields.find("worldMatrix"), iv = fields.find("inverseViewMatrix");
                const pyre::Regex form("mov\\s+r\\d+\\.[xyzw],\\s*" + Escape(reg) + "\\.w");
                if (row >= 3 || reads.empty() || !std::all_of(reads.begin(), reads.end(), [&](const std::string& l) { return form.FullMatch(l).has_value(); }))
                    throw ShaderError("inverse world-view matrix needs a full matrix adapter");
                if (wm == fields.end() || iv == fields.end() || wm->second.size != 64 || iv->second.size != 64)
                    throw ShaderError("inverse world-view camera inputs absent");
                std::string w[3], v[3];
                for (int i = 0; i < 3; i++)
                {
                    w[i] = "t6_cb" + std::to_string(wm->second.slot) + "[" + std::to_string(wm->second.offset / 16 + i) + "]";
                    v[i] = "t6_cb" + std::to_string(iv->second.slot) + "[" + std::to_string(iv->second.offset / 16 + i) + "]";
                }
                std::string columns[3];
                for (int c = 0; c < 3; c++)
                {
                    const char ch = "xyz"[c];
                    columns[c] = "float3(" + w[0] + "." + ch + ", " + w[1] + "." + ch + ", " + w[2] + "." + ch + ")";
                }
                const auto delta = "float3(" + v[0] + ".w - " + w[0] + ".w, " + v[1] + ".w - " + w[1] + ".w, " + v[2] + ".w - " + w[2] + ".w)";
                const auto r = static_cast<int>(row);
                const auto cofactor = "cross(" + columns[(r + 1) % 3] + ", " + columns[(r + 2) % 3] + ")";
                const auto determinant = "dot(" + columns[0] + ", cross(" + columns[1] + ", " + columns[2] + "))";
                Json uses = Json::array();
                for (int i = 0; i < 3; i++)
                    uses.push_back(Json::array({wm->second.slot, wm->second.offset / 16 + i}));
                for (int i = 0; i < 3; i++)
                    uses.push_back(Json::array({iv->second.slot, iv->second.offset / 16 + i}));
                contract["constants"][reg] = Json{{"expr", "float4(0, 0, 0, dot(" + cofactor + ", " + delta + ") / " + determinant + ")"}, {"uses", uses}};
                continue;
            }
            if (arg && AFFINE_INVERSES.count(sourceName) && !fields.count(sourceName))
            {
                const auto row = rowOf();
                const auto inverse = fields.find(AFFINE_INVERSES.at(sourceName));
                if (inverse == fields.end() || inverse->second.size != 64)
                    throw ShaderError(sourceName + " input absent");
                contract["constants"][reg] = AffineInverseRow(inverse->second, static_cast<int>(row));
                continue;
            }
            if (arg && (sourceName == "clipSpaceLookupScale" || sourceName == "clipSpaceLookupOffset"))
            {
                contract["constants"][reg] = Json{{"expr", sourceName == "clipSpaceLookupScale" ? "float4(0.5, -0.5, 1, 1)" : "float4(0.5, 0.5, 0, 0)"}, {"uses", Json::array()}};
                continue;
            }
            if (arg && NEUTRAL_CODE_UNIFORMS.count(sourceName) && !fields.count(sourceName))
            {
                contract["constants"][reg] = Json{{"expr", NEUTRAL_CODE_UNIFORMS.at(sourceName)}, {"uses", Json::array()}};
                AddTo(contract, "neutral", sourceName);
                continue;
            }
            const auto name = !fields.count(sourceName) && CODE_UNIFORM_ALIASES.count(sourceName) ? CODE_UNIFORM_ALIASES.at(sourceName) : sourceName;
            if (!arg || !named || !fields.count(name))
                throw ShaderError("native vertex uniform absent for " + reg + ": " + RegisterRepr(named));
            const auto row = rowOf();
            const auto field = fields.at(name);
            const long long at = field.offset + row * 16;
            if (at % 16 || field.size < (row + 1) * 16)
                throw ShaderError("incompatible vertex uniform " + name);
            if (field.slot != 0)
            {
                bool maintained = false;
                for (const auto& a : nativePass["args"])
                    maintained |= I(a["type"]) == 3 && I(a["buffer"]) == field.slot && I(a["location"]) <= at && at + 16 <= I(a["location"]) + I(a["size"]);
                if (!maintained)
                    throw ShaderError("native pass does not maintain vertex uniform " + name);
            }
            contract["constants"][reg] = Json{{"buffer", field.slot}, {"index", at / 16}};
            if (paired && sourceName == "baseLightingCoords" && name == "gridLightingCoordsAndVis" && field.size >= 16)
            {
                // T6 keeps sun visibility in gridLightingCoordsAndVis.w; carry .w on
                // the varying WaW uses for these coords, when WaW leaves it free
                const auto hits = TaintedOutputs(sourceAssembly, {reg});
                std::set<std::string> carriers;
                std::set<char> comps;
                for (const auto& [r, c] : hits)
                    carriers.insert(r), comps.insert(c);
                std::optional<std::string> carrier;
                if (carriers.size() == 1 && std::all_of(comps.begin(), comps.end(), [](const char c) { return c == 'x' || c == 'y' || c == 'z'; }) &&
                    !WritesComponent(sourceAssembly, *carriers.begin(), 'w'))
                    carrier = *carriers.begin();
                else
                {
                    std::vector<std::string> outputs;
                    for (const auto& [r, _] : contract["outputs"].items())
                        outputs.push_back(r);
                    std::sort(outputs.begin(), outputs.end());
                    for (const auto& r : outputs)
                    {
                        const auto& e = contract["outputs"][r];
                        const auto sem = Upper(e["semantic"].get<std::string>());
                        if (sem != "POSITION" && sem != "POSITION0" && sem != "SV_POSITION" && e["width"].get<int>() <= 3 && !WritesComponent(sourceAssembly, r, 'w'))
                        {
                            carrier = r;
                            break;
                        }
                    }
                }
                if (carrier)
                {
                    contract["outputs"][*carrier]["width"] = 4;
                    contract["epilogue"] = Json{{"lines", Json::array({*carrier + ".w = t6_cb" + std::to_string(field.slot) + "[" + std::to_string(at / 16) + "].w;"})},
                                                {"uses", Json::array({Json::array({field.slot, at / 16})})}};
                    contract["lighting_visibility"] = contract["outputs"][*carrier]["semantic"];
                }
            }
        }
        if (!ir["samplers"].empty())
            throw ShaderError("vertex texture binding adapter required");
        return contract;
    }

    std::string NativeFogAdapter(const std::string& hlsl, const std::string& sourceAssembly, const Json& contract, const Json& nativePass, const fs::path& nativeRoot)
    {
        const auto found = FogOutput(sourceAssembly, contract);
        if (!found)
            return hlsl;
        const auto& [fogReg, fogComp] = *found;
        const auto& fields = Buffers(NativeProgram(nativeRoot, "vs", nativePass));
        const auto field = [&](const std::string& name, const int row = 0, const int channels = 4)
        {
            const auto it = fields.find(name);
            if (it == fields.end())
                throw std::runtime_error("KeyError: " + gsc::Repr(name));
            if (it->second.offset % 16 || it->second.size < row * 16 + channels * 4)
                throw ShaderError("incompatible native fog uniform " + name);
            return "t6_cb" + std::to_string(it->second.slot) + "[" + std::to_string(it->second.offset / 16 + row) + "]";
        };
        std::string position;
        for (const auto& [reg, e] : contract["inputs"].items())
            if (Upper(e["semantic"].get<std::string>()) == "POSITION0")
            {
                position = reg;
                break;
            }
        if (position.empty())
            throw std::runtime_error("StopIteration");
        const std::string wm[3] = {field("worldMatrix", 0), field("worldMatrix", 1), field("worldMatrix", 2)};
        const auto fc = field("fogColor"), f = field("fogConsts"), f2 = field("fogConsts2"), sc = field("sunFogColor");
        const auto sd = field("sunFogDir", 0, 3), sf = field("sunFog", 0, 2);
        const auto p = "float4(" + position + ".xyz,1)";
        std::string code = "\n    float3 fog_p = float3(dot(" + p + ", " + wm[0] + "), dot(" + p + ", " + wm[1] + "), dot(" + p + ", " + wm[2] + "));\n" +
                           "    float fog_dist = length(fog_p);\n" + "    float3 fog_dir = fog_p * rsqrt(dot(fog_p,fog_p));\n" + "    float fog_sun = saturate(dot(" + sd +
                           ".xyz, fog_dir)*" + sf + ".y+" + sf + ".x);\n" + "    float fog_alpha = lerp(" + fc + ".w, " + sc + ".w, fog_sun);\n" +
                           "    float fog_height = " + f + ".w*fog_p.z+" + f + ".x;\n" + "    float fog_integral = (fog_height<0 ? exp2(min(fog_height,64)*1.442695) : fog_height+1)-" + f2 +
                           ".x;\n" + "    float fog_denom = fog_p.z*" + f + ".w;\n" + "    float fog_ratio = abs(fog_denom)<0.0001 ? saturate(" + f2 + ".x) : fog_integral/fog_denom;\n" +
                           "    float fog_density = fog_ratio*" + f + ".y*fog_dist+" + f + ".z;\n" + "    " + fogReg + "." + fogComp + " = 1-(1-min(exp2(fog_density),1))*fog_alpha;\n";
        if (contract.contains("sky_fog_distance"))
        {
            std::string sky = "float3(";
            for (int i = 0; i < 3; i++)
                sky += (i ? ", " : "") + std::string("dot(") + position + ".xyz, " + wm[i] + ".xyz)";
            sky += ")";
            const auto start = code.find("    float fog_dist");
            code = "\n    float3 fog_p = normalize(" + sky + ") * " + F(contract["sky_fog_distance"].get<double>()) + ";\n" + code.substr(start);
        }
        std::string out = hlsl;
        const std::string marker = "Output output;";
        const auto replacement = code + "\nOutput output;";
        for (size_t at = 0; (at = out.find(marker, at)) != std::string::npos; at += replacement.size())
            out.replace(at, marker.size(), replacement);
        return out;
    }

    void CheckLinkage(const std::string& vertexCode, const std::string& pixelCode)
    {
        const auto outputs = CompiledSignature(vertexCode, "Output");
        for (const auto& [key, value] : CompiledSignature(pixelCode, "Input"))
        {
            const auto it = outputs.find(key);
            const auto& [reg, mask] = value;
            const bool ok = it != outputs.end() && it->second.first == reg && std::all_of(mask.begin(), mask.end(), [&](const char c) { return it->second.second.find(c) != std::string::npos; });
            if (!ok)
            {
                const auto sep = key.find('\x1f');
                const auto keyRepr = "(" + gsc::Repr(key.substr(0, sep)) + ", " + key.substr(sep + 1) + ")";
                const auto foundRepr = it == outputs.end() ? std::string("None") : "(" + std::to_string(it->second.first) + ", " + gsc::Repr(it->second.second) + ")";
                throw ShaderError("compiled stages do not link at " + keyRepr + ": vs " + foundRepr + " ps (" + std::to_string(reg) + ", " + gsc::Repr(mask) + ")");
            }
        }
    }

    void PruneMissingMaterialConstants(Json& nativePass, const materials::T6Material& material, const std::vector<const Json*>& contracts, const bool unread)
    {
        std::set<long long> hashes;
        for (const auto& c : material.constants)
            hashes.insert(c.nameHash ? static_cast<long long>(*c.nameHash) : T6Hash(c.name.value_or(std::string())));
        Json args = Json::array();
        size_t cursor = 0;
        std::map<std::string, long long> counts;
        for (const char* count : {"perPrimArgCount", "perObjArgCount", "stableArgCount"})
        {
            const auto n = static_cast<size_t>(I(nativePass[count]));
            long long kept = 0;
            for (size_t i = cursor; i < std::min(cursor + n, nativePass["args"].size()); i++)
            {
                const auto& arg = nativePass["args"][i];
                if (I(arg["type"]) == 0 || I(arg["type"]) == 6)
                {
                    const auto start = I(arg["location"]), end = start + I(arg["size"]);
                    bool read = false;
                    for (const auto* contract : contracts)
                        for (const auto& [_, binding] : (*contract)["constants"].items())
                        {
                            std::vector<std::pair<long long, long long>> uses;
                            for (const auto& u : binding.value("uses", Json::array()))
                                uses.emplace_back(I(u[0]), I(u[1]));
                            if (binding.contains("buffer"))
                                uses.emplace_back(I(binding["buffer"]), I(binding["index"]));
                            for (const auto& [buffer, row] : uses)
                                read |= buffer == I(arg["buffer"]) && start <= row * 16 && row * 16 < end;
                        }
                    if (!hashes.count(I(arg["u"]["value"])))
                    {
                        if (read)
                            throw ShaderError("translated shader reads an absent donor material constant");
                        continue;
                    }
                    if (unread && !read)
                        continue;
                }
                args.push_back(arg);
                kept++;
            }
            cursor += n;
            counts[count] = kept;
        }
        if (cursor != nativePass["args"].size())
            throw ShaderError("native argument frequency counts do not cover all bindings");
        for (const auto& [k, v] : counts)
            nativePass[k] = v;
        nativePass["args"] = args;
    }
} // namespace w2b::shaders::runtime
