// WaW visions, the LUT atlas and the film overlay (see visions.h).
#include "visions.h"

#include "core/asset_paths.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/regex.h"
#include "core/zip.h"
#include "materials/t6_material.h"
#include "shaders/techsets.h"
#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "gsc/tokens.h"
#include "materials/techsets.h"
#include "shaders/d3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace w2b::visions
{
    namespace fs = std::filesystem;
    using util::Json;

    namespace
    {
        constexpr double LUMA[3] = {.299, .587, .114};
        constexpr double SHOULDER_KNEE = 0.75, SHOULDER0 = 4.328085, SHOULDER1 = 5.77078;
        constexpr int SCENE_CODE_TEXTURE = 9;
        const std::vector<std::string> USER_DVARS{"r_contrast", "r_brightness", "r_desaturation"};
        const std::vector<std::pair<std::string, std::string>> FILM_TWEAKS{{"r_filmtweakenable", "r_filmenable"},
                                                                           {"r_filmtweakcontrast", "r_filmcontrast"},
                                                                           {"r_filmtweakbrightness", "r_filmbrightness"},
                                                                           {"r_filmtweakdesaturation", "r_filmdesaturation"},
                                                                           {"r_filmtweakinvert", "r_filminvert"},
                                                                           {"r_filmtweakdarktint", "r_filmdarktint"},
                                                                           {"r_filmtweaklighttint", "r_filmlighttint"}};
        const std::string FILM_TWEAK_GRADE = "_filmtweak";

        std::string Str(fs::path p)
        {
            return files::Utf8(p.make_preferred());
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        bool StartsWith(std::string_view s, std::string_view p)
        {
            return s.substr(0, p.size()) == p;
        }

        std::string Bytes(const fs::path& p)
        {
            std::string data;
            if (!files::ReadBytes(p, data))
                throw std::runtime_error("cannot read " + Str(p));
            return data;
        }

        Json ReadJson(const fs::path& p)
        {
            return Json::parse(files::ReadText(p));
        }

        void SortPaths(std::vector<fs::path>& paths)
        {
            std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
        }

        std::vector<std::string> SplitWhite(std::string_view text)
        {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < text.size())
            {
                while (i < text.size() && util::IsSpace(text[i]))
                    i++;
                size_t j = i;
                while (j < text.size() && !util::IsSpace(text[j]))
                    j++;
                if (j > i)
                    out.emplace_back(text.substr(i, j - i));
                i = j;
            }
            return out;
        }

        double PyFloat(const std::string& text)
        {
            double v = 0;
            if (!util::ParseFloat(text, v))
                throw std::runtime_error("could not convert string to float: " + gsc::Repr(text));
            return v;
        }

        double Scalar(const Json& fields, const char* key, const double fallback)
        {
            if (!fields.contains(key))
                return fallback;
            const auto& v = fields[key];
            if (v.is_number())
            {
                const double d = v.get<double>();
                if (!std::isfinite(d))
                    throw std::runtime_error(std::string("non-finite vision value: ") + key);
                return d;
            }
            return VisionFloat(v.get<std::string>());
        }

        std::array<double, 3> Vector(const Json& fields, const char* key)
        {
            const auto parts = SplitWhite(fields.contains(key) ? fields[key].get<std::string>() : std::string("1 1 1"));
            if (parts.size() != 3)
                throw std::runtime_error(std::string("invalid vision vector ") + key);
            std::array<double, 3> v{};
            for (int i = 0; i < 3; i++)
            {
                v[i] = PyFloat(parts[static_cast<size_t>(i)]);
                if (!std::isfinite(v[i]))
                    throw std::runtime_error(std::string("invalid vision vector ") + key);
            }
            return v;
        }

        std::array<double, 3> Film(const std::array<double, 3>& rgb, const Json& fields)
        {
            if (!Scalar(fields, "r_filmenable", 1))
                return rgb;
            const auto [contrast, brightness, desat] = FilmTerms(fields);
            const auto dark = Vector(fields, "r_filmdarktint"), light = Vector(fields, "r_filmlighttint");
            double lum = 0;
            for (int i = 0; i < 3; i++)
                lum += rgb[i] * LUMA[i];
            double scale = contrast, bias = brightness + .5 - contrast * .5;
            if (Scalar(fields, "r_filminvert", 0))
            {
                scale = -scale;
                bias += 1;
            }
            std::array<double, 3> out{};
            for (int i = 0; i < 3; i++)
                out[i] = std::max(0., std::min(1., ((1 - desat) * rgb[i] + desat * lum) * (dark[i] + (light[i] - dark[i]) * lum) * scale + bias));
            return out;
        }

        double Unshoulder(const double c)
        {
            if (c <= SHOULDER_KNEE)
                return c;
            const double rest = 1 - (c - SHOULDER_KNEE) / (1 - SHOULDER_KNEE);
            if (rest <= 0)
                return 1.;
            return std::min(1., (SHOULDER0 - std::log(rest)) / SHOULDER1);
        }

        // parse(): comments removed, key "value" pairs, keys lower-cased
        Json ParseVision(const std::string& text)
        {
            static const regex::Regex comments("/\\*.*?\\*/|//[^\\n]*", regex::DOTALL);
            static const regex::Regex pair("([\\w]+)\\s+\"([^\"\\r\\n]*)\"");
            Json out = Json::object();
            for (const auto& m : pair.FindAll(comments.Sub("", text)))
                out[Lower(m[0])] = m[1];
            return out;
        }

        Json NeutralVisionFields(const Json& original)
        {
            Json out{{"r_filmEnable", "1"}, {"r_filmLut", "-1"}, {"vc_RS", "0 .5 0 .75"}, {"vc_RE", ".5 1 .25 1"}, {"vc_FGM", ".5 .5 .5 1"},
                     {"vc_FSM", ".212585 .715195 .072220 1"}, {"vc_FBM", "0 0 0 0"}, {"vc_LIB", "0 0 0 0"}, {"vc_LIW", "32 32 32 32"},
                     {"vc_LIG", "1 1 1 1"}, {"vc_LOB", "0 0 0 0"}, {"vc_LOW", "32 32 32 32"}, {"vc_RGBH", "0 0 0 0"}, {"vc_RGBL", "0 0 0 0"},
                     {"vc_YH", "0 0 0 0"}, {"vc_YL", "0 0 0 0"}};
            for (const char band : {'S', 'M', 'H'})
            {
                const char* vecs[] = {"1 0 0 0", "0 1 0 0", "0 0 1 0"};
                for (int c = 0; c < 3; c++)
                    out[std::string("vc_") + band + "M" + "RGB"[c]] = vecs[c];
            }
            // shared fields verified in T6's vision parser table at 0xD3E4F8
            for (const char* key : {"r_primarylightusetweaks", "r_primarylighttweakdiffusestrength", "r_primarylighttweakspecularstrength", "snd_masterringmod",
                                    "snd_reverbringmod", "snd_hifilter", "snd_lowfilter", "r_sunflaretint", "r_postemissivebrightening"})
                if (original.contains(key))
                    out[key] = original[key];
            out["r_reviveFX_enable"] = original.contains("r_revivefx_enable") ? original["r_revivefx_enable"] : Json("0");
            return out;
        }

        std::string NeutralVision(const Json& original)
        {
            std::string text = "// WaW film baked into the map LUT atlas.\n";
            const auto fields = NeutralVisionFields(original);
            for (const auto& [k, v] : fields.items())
                text += k + " \"" + v.get<std::string>() + "\"\n";
            return text;
        }

        // script_dvars: the last literal each script sets per renderer dvar
        Json ScriptDvars(const std::vector<fs::path>& paths)
        {
            Json out = Json::object();
            for (const auto& path : paths)
            {
                const auto tokens = gsc::Tokenize(encoding::UniversalNewlines(encoding::Utf8Replace(Bytes(path))));
                for (size_t i = 0; i < tokens.size(); i++)
                {
                    if (Lower(tokens[i].text).find("setclientdvar") == std::string::npos || i + 1 >= tokens.size() || tokens[i + 1].text != "(")
                        continue;
                    size_t j = i + 2;
                    while (j + 2 < tokens.size() && tokens[j].kind == gsc::Kind::String && tokens[j + 1].text == "," &&
                           (tokens[j + 2].kind == gsc::Kind::String || tokens[j + 2].kind == gsc::Kind::Number))
                    {
                        const auto& value = tokens[j + 2].text;
                        const auto& name = tokens[j].text;
                        out[Lower(name.substr(1, name.size() - 2))] = tokens[j + 2].kind == gsc::Kind::String ? value.substr(1, value.size() - 2) : value;
                        j += 3;
                        if (j < tokens.size() && tokens[j].text == ",")
                            j++;
                        else
                            break;
                    }
                }
            }
            return out;
        }

        // glow._num: float(fields.get(key, default)), the default when that fails
        double Num(const Json& fields, const char* key, const double fallback)
        {
            if (!fields.contains(key))
                return fallback;
            const auto& v = fields[key];
            if (v.is_number())
                return v.get<double>();
            double d = 0;
            if (v.is_string() && util::ParseFloat(v.get<std::string>(), d))
                return d;
            return fallback;
        }

        std::string F(const double v)
        {
            return util::FloatRepr(v);
        }

        std::string F3(const std::array<double, 3>& v)
        {
            return "float3(" + F(v[0]) + ", " + F(v[1]) + ", " + F(v[2]) + ")";
        }

        // glow.pixel_hlsl for the film-only pass (WaW glow is not reproduced)
        std::string PixelHlsl(const Json& fields)
        {
            std::vector<std::string> lines{"Texture2D<float4> scene : register(t0);", "SamplerState scene_sampler : register(s0);",
                                           "static const float3 LUMA = " + F3({LUMA[0], LUMA[1], LUMA[2]}) + ";", "float3 film(float3 x)", "{"};
            if (Num(fields, "r_filmenable", 1))
            {
                const auto [contrast, brightness, desat] = FilmTerms(fields);
                double scale = contrast, bias = brightness + .5 - contrast * .5;
                if (Num(fields, "r_filminvert", 0))
                    scale = -scale, bias = bias + 1;
                const auto vector = [&](const char* key)
                {
                    const auto parts = SplitWhite(fields.contains(key) ? fields[key].get<std::string>() : std::string("1 1 1"));
                    if (parts.size() != 3)
                        throw std::runtime_error(std::string("invalid vision vector ") + key);
                    return std::array<double, 3>{PyFloat(parts[0]), PyFloat(parts[1]), PyFloat(parts[2])};
                };
                const auto dark = vector("r_filmdarktint"), light = vector("r_filmlighttint");
                lines.push_back("    float lum = dot(x, LUMA);");
                lines.push_back("    float3 tint = " + F3(dark) + " + (" + F3(light) + " - " + F3(dark) + ") * lum;");
                lines.push_back("    return ((1.0 - " + F(desat) + ") * x + " + F(desat) + " * lum) * tint * " + F(scale) + " + " + F(bias) + ";");
            }
            else
                lines.push_back("    return x;");
            for (const char* l : {"}", "", "float4 keep_2d(float3 x, float3 y)", "{", "    float3 r = x > 0.0 ? y / max(x, 1e-6) : 1.0;",
                                  "    float a = saturate(min(r.x, min(r.y, r.z)));", "    return float4(saturate(y - a * x), a);", "}", "",
                                  "float4 main(float4 pos : SV_Position) : SV_Target0", "{", "    float3 x = scene.Load(int3(int2(pos.xy), 0)).rgb;",
                                  "    return keep_2d(x, saturate(film(x)));", "}"})
                lines.emplace_back(l);
            std::string text;
            for (size_t i = 0; i < lines.size(); i++)
                text += (i ? "\n" : "") + lines[i];
            return text + "\n";
        }

        // glow.stage: one overlay material per vision (plus the identity vision)
        Json StageGlow(const fs::path& project, const Json& grading, const fs::path& techsetDump)
        {
            std::optional<shaders::T6TechniqueSet> base;
            {
                std::vector<fs::path> found;
                std::error_code e;
                for (const auto& entry : fs::directory_iterator(techsetDump / "techniquesets", e))
                    if (Lower(files::Utf8(entry.path().extension())) == ".json")
                        found.push_back(entry.path());
                SortPaths(found);
                for (const auto& p : found)
                {
                    const auto parsed = materials::ParseTechnique(files::Utf8(p.stem()));
                    if (parsed.family != materials::Family::other || parsed.lit || parsed.unlit != "trivial")
                        continue;
                    auto data = shaders::ReadT6TechniqueSet(p);
                    const auto techniques = std::ranges::count_if(data.techniques, [](const auto& t) { return t.technique.has_value(); });
                    const auto first = std::ranges::find_if(data.techniques, [](const auto& t) { return t.technique.has_value(); });
                    if (techniques == 1 && first->technique->passes.size() == 1)
                    {
                        base = std::move(data);
                        break;
                    }
                }
                if (!base)
                    throw std::runtime_error("no stock T6 trivial HUD technique set in the technique dump");
            }
            std::optional<materials::T6Material> donor;
            {
                auto found = files::RGlob(techsetDump / "materials", ".json");
                SortPaths(found);
                for (const auto& p : found)
                {
                    const auto data = ReadJson(p);
                    if (data.value("techniqueSet", std::string()) == base->name)
                    {
                        donor = materials::T6MaterialFromJson(data);
                        break;
                    }
                }
                if (!donor)
                    throw std::runtime_error("no stock material uses " + base->name);
            }
            size_t slot = 0;
            while (!base->techniques[slot].technique)
                slot++;
            Json report{{"overlays", Json::object()}, {"glow", Json::object()}, {"notes", Json::object()}, {"materials", Json::array()}, {"technique_donor", base->name}};
            std::map<std::string, Json> visions{{"_identity", Json{{"r_filmenable", "0"}}}};
            for (const auto& [name, fields] : grading.items())
                visions[name] = fields;
            for (const auto& [vision, fields] : visions)
            {
                Json notes = Json::array();
                // glow_constants: WaW draws glow only with r_glow and a non-zero intensity and radius
                const bool glows = Num(fields, "r_glow", 0) && Num(fields, "r_glowbloomintensity0", 0) != 0 && Num(fields, "r_glowradius0", 0) != 0;
                if (glows)
                {
                    if (Num(fields, "r_glowrayintensity", 0) > 0)
                        notes.push_back("god rays (r_glowRayIntensity) not reproduced");
                    notes.push_back("WaW glow not reproduced (exact single-pass glow too slow)");
                }
                const auto code = shaders::CompileHlsl(PixelHlsl(fields), "ps_5_0");
                const auto shader = "waw/overlay_" + hash::Sha256Hex(code).substr(0, 20);
                const auto target = project / "shader_bin" / files::FromUtf8("ps_" + shader + ".cso");
                fs::create_directories(target.parent_path());
                files::WriteBytes(target, code);
                auto technique = *base;
                technique.name = shader;
                auto& tech = *technique.techniques[slot].technique;
                tech.name = shader + "_" + std::to_string(slot);
                auto& p = tech.passes[0];
                p.pixelShader = shader;
                // the scene texture replaces the material colour map; it is a stable argument
                const auto removed = static_cast<long long>(std::erase_if(p.args, [](const shaders::T6PassArg& a) { return a.type == 2; }));
                p.args.push_back({0, 0, 1, 4, SCENE_CODE_TEXTURE});
                p.stableArgCount = p.stableArgCount + 1 - removed;
                const auto path = project / "techniquesets" / files::FromUtf8(shader + ".json");
                fs::create_directories(path.parent_path());
                files::WriteText(path, util::JsonDumps(shaders::ToJson(technique), 2));
                auto material = *donor;
                material.techniqueSet = shader;
                for (auto& texture : material.textures)
                {
                    auto image = texture.image;
                    if (StartsWith(image, ","))
                        image.erase(0, 1);
                    texture.image = StartsWith(image, "$") ? "," + image : image;
                }
                for (auto& state : material.stateBits)
                {
                    state.alphaTest = "disabled";
                    state.blendOpRgb = "add", state.srcBlendRgb = "one", state.dstBlendRgb = "srcalpha";
                    state.blendOpAlpha = "add", state.srcBlendAlpha = "zero", state.dstBlendAlpha = "one";
                }
                auto name = vision;
                std::replace(name.begin(), name.end(), '/', '_');
                name = "waw/vision_overlay/" + name;
                const auto out = project / assets::MaterialPath(name);
                fs::create_directories(out.parent_path());
                files::WriteText(out, materials::T6MaterialText(material));
                report["overlays"][vision] = name;
                report["materials"].push_back(name);
                report["glow"][vision] = nullptr;
                if (!notes.empty())
                    report["notes"][vision] = notes;
            }

            return report;
        }

        // lut_donor: the native LUT material, found by renderer role
        std::optional<fs::path> LutDonor(const std::optional<fs::path>& root)
        {
            if (!root)
                return std::nullopt;
            auto found = files::RGlob(*root / "materials", ".json");
            SortPaths(found);
            for (const auto& p : found)
            {
                const auto material = ReadJson(p);
                if (StartsWith(material.value("techniqueSet", std::string()), "hdr_create_lut2dv_") && material.contains("textures") && !material["textures"].empty())
                    return p;
            }
            return std::nullopt;
        }
    } // namespace

    double VisionFloat(const std::string& value)
    {
        double result = 0;
        if (!util::ParseFloat(value, result))
        {
            static const regex::Regex prefix("^[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?");
            size_t i = 0;
            while (i < value.size() && util::IsSpace(value[i]))
                i++;
            const auto match = prefix.MatchAt(value.substr(i));
            if (!match)
                throw std::runtime_error("invalid numeric vision value: " + gsc::Repr(value));
            result = PyFloat((*match)[0]);
        }
        if (!std::isfinite(result))
            throw std::runtime_error("non-finite vision value: " + gsc::Repr(value));
        return result;
    }

    std::tuple<double, double, double> FilmTerms(const Json& fields)
    {
        const double d = Scalar(fields, "r_filmdesaturation", 0);
        return {Scalar(fields, "r_filmcontrast", 1) * Scalar(fields, "r_contrast", 1), Scalar(fields, "r_filmbrightness", 0) + Scalar(fields, "r_brightness", 0),
                std::max(1. / 4096, d * (d + (1 - d) * Scalar(fields, "r_desaturation", 0)))};
    }

    std::string Atlas(const Json& grades)
    {
        // x = blue*32 + red, y = grade*32 + green; each grid point undoes T6's
        // highlight shoulder, then applies the WaW film
        const uint16_t width = 1024, height = static_cast<uint16_t>(32 * grades.size());
        std::string data;
        data.reserve(static_cast<size_t>(width) * height * 4);
        double table[32];
        for (int i = 0; i < 32; i++)
            table[i] = Unshoulder(i / 31.0);
        for (const auto& grade : grades)
            for (int green = 0; green < 32; green++)
                for (int blue = 0; blue < 32; blue++)
                    for (int red = 0; red < 32; red++)
                    {
                        const auto rgb = Film({table[red], table[green], table[blue]}, grade);
                        for (const double x : rgb)
                            data += static_cast<char>(static_cast<int>(std::nearbyint(x * 255)));
                        data += static_cast<char>(255);
                    }
        std::string out = "IWi\x1b";
        const auto put = [&](const void* p, const size_t n) { out.append(static_cast<const char*>(p), n); };
        const int8_t format = 1, flags = 2;
        const uint16_t depth = 1;
        const float zero = 0;
        put(&format, 1), put(&flags, 1), put(&width, 2), put(&height, 2), put(&depth, 2), put(&zero, 4);
        out.append(16, '\0');
        const uint32_t sizes[8] = {static_cast<uint32_t>(64 + data.size())};
        put(sizes, sizeof sizes);
        return out + data;
    }

    Json StageVisions(const VisionRequest& request)
    {
        const auto& project = request.project;
        const auto& sourceRoot = request.sourceRoot;
        const auto& techsetDump = request.techsetDump;
        auto* stock = request.stock;
        std::map<std::string, std::pair<std::string, std::string>> sources;
        for (const auto& archive : request.iwds)
        {
            zip::Archive z(archive);
            std::vector<const zip::Entry*> entries;
            for (const auto& entry : z.Entries())
                entries.push_back(&entry);
            std::sort(entries.begin(), entries.end(), [](const auto* x, const auto* y) { return x->name < y->name; });
            for (const auto* entry : entries)
            {
                auto low = Lower(entry->name);
                std::replace(low.begin(), low.end(), '\\', '/');
                if (StartsWith(low, "vision/") && low.size() >= 14 && low.substr(low.size() - 7) == ".vision")
                    sources.try_emplace(low.substr(7, low.size() - 14), encoding::Utf8Replace(z.Read(*entry)), Str(archive) + ":" + entry->name);
            }
        }
        for (const auto& root : request.roots)
        {
            auto found = files::RGlob(root / "vision", ".vision");
            SortPaths(found);
            for (const auto& p : found)
            {
                auto relative = files::Utf8(p.lexically_relative(root / "vision"));
                std::replace(relative.begin(), relative.end(), '\\', '/');
                sources.try_emplace(Lower(relative.substr(0, relative.size() - 7)), encoding::UniversalNewlines(encoding::Utf8Replace(Bytes(p))), Str(p));
            }
        }
        std::set<std::string> wanted;
        auto scripts = files::RGlob(project / "maps/mp/waw", ".gsc");
        for (const auto& p : scripts)
        {
            const auto tokens = gsc::Tokenize(encoding::WawText(Bytes(p)));
            for (size_t i = 0; i < tokens.size(); i++)
            {
                const auto& t = tokens[i];
                if (t.kind != gsc::Kind::String)
                    continue;
                const auto value = Lower(t.text.size() >= 2 ? t.text.substr(1, t.text.size() - 2) : std::string());
                if (sources.count(value))
                    wanted.insert(value);
                // literal first arguments to all vision APIs, including missing ones
                if (i >= 2 && tokens[i - 1].text == "(" && Lower(tokens[i - 2].text).find("vision") != std::string::npos && !value.empty())
                    wanted.insert(value);
            }
        }
        Json report{{"status", "native_lut"},
                    {"visions", Json::object()},
                    {"missing", Json::array()},
                    {"not_translated", Json::object()},
                    {"sampling", "32^3 RGB8, native T6 spatial LUT interpolation"},
                    {"transition", "LUT grade switches immediately; shared vision fields retain native fade time"},
                    {"errors", Json::array()},
                    {"numeric_prefixes", Json::array()}};
        const auto lookup = [&](const std::string& name)
        {
            if (!sources.count(name) && stock)
                if (const auto hit = stock->RootFor("rawfile", "vision/" + name + ".vision"))
                {
                    const auto path = hit->second / "vision" / files::FromUtf8(name + ".vision");
                    if (fs::exists(path))
                        sources[name] = {encoding::WawText(Bytes(path)), Str(path)};
                }
            if (!sources.count(name) && sourceRoot)
            {
                const auto path = *sourceRoot / "vision" / files::FromUtf8(name + ".vision");
                if (IsFile(path))
                    sources[name] = {encoding::WawText(Bytes(path)), Str(path)};
            }
            return sources.count(name) > 0;
        };
        report["default_fallback"] = Json::array();
        for (const auto& name : wanted)
        {
            if (lookup(name))
                continue;
            // WaW cannot open the file and loads vision/default instead
            if (name != "default" && lookup("default"))
            {
                sources[name] = sources["default"];
                report["default_fallback"].push_back(name);
            }
            else
                report["missing"].push_back(name);
        }
        Json grades = Json::array({Json{{"r_filmenable", "0"}}});
        std::vector<std::string> init{"// WaW vision names and their translated rawfiles.", "init()", "{", "    level.waw2bo2_visions = [];", "    level.waw2bo2_vision_indices = [];"};
        std::string zoneLines;
        const auto identity = project / "vision/waw/_identity.vision";
        fs::create_directories(identity.parent_path());
        files::WriteText(identity, NeutralVision(Json::object()));
        zoneLines += "rawfile,vision/waw/_identity.vision\n";
        SortPaths(scripts);
        const auto dvars = ScriptDvars(scripts);
        Json user = Json::object();
        for (const auto& k : USER_DVARS)
            if (dvars.contains(k))
                user[k] = dvars[k];
        report["script_render_dvars"] = Json::object();
        for (const auto& [k, v] : dvars.items())
            if (StartsWith(k, "r_") || StartsWith(k, "sm_"))
                report["script_render_dvars"][k] = v;
        static const regex::Regex validName("[a-z0-9_/.-]+");
        static const regex::Regex prefix("^[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?");
        for (const auto& name : wanted)
        {
            if (!sources.count(name))
                continue;
            if (!validName.FullMatch(name) || name.find("..") != std::string::npos)
            {
                report["errors"].push_back("invalid vision name: " + name);
                continue;
            }
            auto fields = ParseVision(sources[name].first);
            for (const auto& [k, v] : user.items())
                fields[k] = v;
            for (auto& [key, value] : fields.items())
            {
                const auto text = value.get<std::string>();
                if (!StartsWith(key, "r_") || SplitWhite(text).size() != 1)
                    continue;
                double ignored = 0;
                if (util::ParseFloat(text, ignored))
                    continue;
                size_t i = 0;
                while (i < text.size() && util::IsSpace(text[i]))
                    i++;
                if (!prefix.MatchAt(text.substr(i)))
                    continue;
                const double parsed = VisionFloat(text);
                value = util::FloatRepr(parsed);
                report["numeric_prefixes"].push_back(Json{{"vision", name}, {"source", sources[name].second}, {"field", key}, {"original", text}, {"parsed", parsed}});
            }
            const auto index = grades.size();
            // vc_LUT's native range is -32..32; it selects abs(value)-1
            if (index >= 32)
            {
                report["errors"].push_back("native T6 LUT capacity exceeded by vision: " + name);
                continue;
            }
            grades.push_back(fields);
            const auto target = project / "vision/waw" / files::FromUtf8(name + ".vision");
            fs::create_directories(target.parent_path());
            files::WriteText(target, NeutralVision(fields));
            init.push_back("    level.waw2bo2_visions[\"" + name + "\"] = \"waw/" + name + "\";");
            init.push_back("    level.waw2bo2_vision_indices[\"" + name + "\"] = " + std::to_string(index) + ";");
            zoneLines += "rawfile,vision/waw/" + name + ".vision\n";
            report["visions"][name] = Json{{"source", sources[name].second}, {"index", index}, {"output", "vision/waw/" + name + ".vision"}};
            Json kept = Json::object();
            for (const auto& [k, v] : fields.items())
                if (StartsWith(k, "r_glow") || (StartsWith(k, "r_revivefx_") && k != "r_revivefx_enable"))
                    kept[k] = v;
            report["not_translated"][name] = kept;
        }
        if (techsetDump)
        {
            Json grading = Json::object();
            size_t i = 1;
            for (const auto& [name, _] : report["visions"].items())
                grading[name] = grades[i++];
            const auto tweaks = dvars.value("r_filmusetweaks", std::string("0"));
            if (tweaks != "0" && !tweaks.empty())
            {
                // r_filmUseTweaks: WaW films with the r_filmTweak* dvars whatever the vision says
                Json tweak{{"r_filmenable", "0"}};
                for (const auto& [k, v] : dvars.items())
                    for (const auto& [from, to] : FILM_TWEAKS)
                        if (k == from)
                            tweak[to] = v;
                for (const auto& [k, v] : user.items())
                    tweak[k] = v;
                grading[FILM_TWEAK_GRADE] = tweak;
                report["film_tweak"] = tweak;
            }
            const auto overlays = StageGlow(project, grading, *techsetDump);
            init.emplace_back("    level.waw2bo2_vision_overlays = [];");
            for (const auto& [name, material] : overlays["overlays"].items())
            {
                init.push_back("    level.waw2bo2_vision_overlays[\"" + name + "\"] = \"" + material.get<std::string>() + "\";");
                init.push_back("    precacheshader( \"" + material.get<std::string>() + "\" );");
            }
            report["overlay"] = overlays;
            // film now runs exactly in the overlay; the LUT keeps the shoulder inverse
            for (auto& g : grades)
                g = Json{{"r_filmenable", "0"}};
        }
        init.emplace_back("}");
        init.emplace_back("");
        std::string script;
        for (size_t i = 0; i < init.size(); i++)
            script += (i ? "\n" : "") + init[i];
        files::WriteText(project / "maps/mp/waw/_waw2bo2_visions.gsc", script);
        if (grades.size() > 1)
        {
            const auto donor = request.donor ? request.donor : LutDonor(techsetDump);
            if (!donor || !fs::exists(*donor))
                report["errors"].push_back("native T6 LUT material donor missing");
            else
            {
                auto material = materials::ReadT6Material(*donor);
                material.textures.at(0).image = "waw_vision_lut";
                fs::create_directories(project / "materials/waw");
                files::WriteText(project / "materials/waw/vision_lut.json", materials::T6MaterialText(material));
                fs::create_directories(project / "images");
                fs::create_directories(project / "BSP");
                files::WriteBytes(project / "images/waw_vision_lut.iwi", Atlas(grades));
                files::WriteText(project / "BSP/visions.json", util::JsonDumpsDefault(Json{{"material", "waw/vision_lut"}, {"grades", grades.size()}}) + "\n");
            }
        }
        {
            // the mod zone lines, appended in text mode
            std::string existing;
            files::ReadBytes(project / "mod_extra.zone", existing);
            std::string add;
            for (const char c : zoneLines)
                add += c == '\n' ? std::string("\r\n") : std::string(1, c);
            files::WriteBytes(project / "mod_extra.zone", existing + add);
        }
        fs::create_directories(project / "content_source");
        files::WriteText(project / "content_source/visions.stage.json", util::JsonDumps(report, 2) + "\n");
        return report;
    }
} // namespace w2b::visions
