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
        // T6's LUT target is R10G10B10A2 (sub_75A9C0, format 0x18): two alpha bits per texel. The active vision's
        // glow intensity is a thermometer code over the alpha of the first GLOW_TEXELS_X x 2 texels of the LUT
        // (3 units each): intensity = mean alpha * the map's largest glow intensity. bloom_apply averages them
        // with bilinear fetches between texel pairs; blended visions blend the code linearly.
        constexpr int GLOW_TEXELS_X = 24, GLOW_UNITS = GLOW_TEXELS_X * 2 * 3;
        // the glow chain follows the vision the level starts with (createart) over the ones scripts switch to
        constexpr size_t CREATEART_WEIGHT = 1000;
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

        std::string Join(const std::vector<std::string>& lines)
        {
            std::string text;
            for (const auto& l : lines)
                text += l + "\n";
            return text;
        }

        // WaW's glow constants (sub_6DC300): drawn only with r_glow and a non-zero intensity and radius
        struct GlowTerms
        {
            double cutoff = 0, desaturation = 0, intensity = 0, radius = 0;
        };

        std::optional<GlowTerms> Glow(const Json& fields)
        {
            const GlowTerms terms{Num(fields, "r_glowbloomcutoff", 0), Num(fields, "r_glowbloomdesaturation", 0), Num(fields, "r_glowbloomintensity0", 0),
                                  Num(fields, "r_glowradius0", 0)};
            if (!Num(fields, "r_glow", 0) || terms.intensity == 0 || terms.radius == 0)
                return std::nullopt;
            return terms;
        }

        // the WaW film of a vision as an HLSL function film(x), with LUMA
        std::vector<std::string> FilmHlsl(const Json& fields)
        {
            std::vector<std::string> lines{"static const float3 LUMA = " + F3({LUMA[0], LUMA[1], LUMA[2]}) + ";", "float3 film(float3 x)", "{"};
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
            lines.emplace_back("}");
            return lines;
        }

        // the vision overlay: the WaW film of the frame
        std::string PixelHlsl(const Json& fields)
        {
            std::vector<std::string> lines{"Texture2D<float4> scene : register(t0);", "SamplerState scene_sampler : register(s0);"};
            for (auto& l : FilmHlsl(fields))
                lines.push_back(std::move(l));
            for (const char* l : {"", "float4 keep_2d(float3 x, float3 y)", "{", "    float3 r = x > 0.0 ? y / max(x, 1e-6) : 1.0;",
                                  "    float a = saturate(min(r.x, min(r.y, r.z)));", "    return float4(saturate(y - a * x), a);", "}", "",
                                  "float4 main(float4 pos : SV_Position) : SV_Target0", "{", "    float3 x = scene.Load(int3(int2(pos.xy), 0)).rgb;",
                                  "    return keep_2d(x, saturate(film(x)));", "}"})
                lines.emplace_back(l);
            return Join(lines);
        }

        // T6's bloom chain (sub_767DE0, default r_bloomHiQuality 0) carries WaW's glow:
        //   bloom_downsample              scene -> RT38 (W/4 x 180)   WaW glow setup
        //   bloom_downsample_convolution  RT38 -> RT40 -> RT42         stock 2x2 box
        //   bloom_blur_y/x                RT42 <-> RT43, RT41 <-> RT40 WaW Gaussian, radius0 in 480-line units
        //   bloom_combine_hilo            RT40 -> RT41                 the RT40 level only
        //   bloom_apply                   stock tonemap and LUT without T6's bloom term
        // bloom_apply adds the glow (code texture 28 is not valid any more when the HUD overlay draws).

        // WaW's setup pass (sub_74B2E0): every tap is a 2x2-pixel average c of the frame, weighted
        // film(c) * saturate(lum(c) - cutoff), summed, scaled by 1 / (1 - cutoff), then desaturated
        std::string GlowSetupHlsl(const Json& film, const GlowTerms& glow)
        {
            std::vector<std::string> lines{"Texture2D<float4> scene : register(t0);", "SamplerState scene_sampler : register(s0);"};
            for (auto& l : FilmHlsl(film))
                lines.push_back(std::move(l));
            const double scale = glow.cutoff < 1 ? 1 / (1 - glow.cutoff) : 0;
            for (const auto& l : std::vector<std::string>{
                     "", "float4 main(float4 pos : SV_Position, float4 taps : TEXCOORD0) : SV_Target0", "{",
                     "    float2 centre = (taps.xy + taps.zw) * 0.5;",
                     "    float2 texel = float2(abs(ddx(centre.x)), abs(ddy(centre.y)));",
                     "    float w, h;",
                     "    scene.GetDimensions(w, h);",
                     "    int2 count = max(int2(round(texel * float2(w, h) * 0.5)), int2(1, 1));",
                     "    float2 step = texel / float2(count);",
                     "    float2 origin = centre - texel * 0.5;",
                     "    float3 sum = 0.0;",
                     "    [loop] for (int y = 0; y < count.y; y++)",
                     "        [loop] for (int x = 0; x < count.x; x++)",
                     "        {",
                     // T6 holds the frame as s with display sqrt(4 s^2) (bloom_apply); the LUT undoes the shoulder
                     "            float3 c = saturate(2.0 * abs(scene.SampleLevel(scene_sampler, origin + (float2(x, y) + 0.5) * step, 0).rgb));",
                     "            sum += saturate(film(c)) * saturate(dot(c, LUMA) - " + F(glow.cutoff) + ");",
                     "        }",
                     "    float3 g = sum / float(count.x * count.y) * " + F(scale) + ";",
                     "    return float4(saturate(lerp(g, dot(g, LUMA), " + F(glow.desaturation) + ")), 1.0);", "}"})
                lines.push_back(l);
            return Join(lines);
        }

        // WaW's blur (IW3 Gaussian, sub_74AA70): sigma = radius * screen height / 480 pixels, on a quarter-res
        // setup (box 4x4 px, bilinear upsample: 4 px^2 more variance). T6's levels add a one-texel box and
        // a bilinear upsample (1/4 texel^2), so the Gaussian here makes up the difference.
        std::string GlowBlurHlsl(const GlowTerms& glow, const bool horizontal)
        {
            const std::string axis = horizontal ? "x" : "y";
            return Join({"Texture2D<float4> source : register(t0);", "Texture2D<float4> frame : register(t1);", "",
                         "float4 main(float4 pos : SV_Position) : SV_Target0", "{",
                         "    float w, h, sw, sh;",
                         "    source.GetDimensions(w, h);",
                         "    frame.GetDimensions(sw, sh);",
                         "    if (sw < 1.0 || sh < 1.0)",
                         "        sw = 1920.0, sh = 1080.0;",
                         "    float2 px = float2(sw / w, sh / h);",
                         "    float2 sigma = " + F(glow.radius) + " * sh / 480.0 / px;",
                         "    float v = (sigma * sigma + 4.0 / (px * px) - 0.25)." + axis + ";",
                         "    int2 p = int2(pos.xy);",
                         "    if (v <= 0.0)",
                         "        return source.Load(int3(p, 0));",
                         "    int n = min(int(ceil(3.0 * sqrt(v))), 32);",
                         "    int2 last = int2(w, h) - 1;",
                         "    float4 sum = 0.0;",
                         "    float total = 0.0;",
                         "    [loop] for (int i = -n; i <= n; i++)",
                         "    {",
                         "        float k = exp(-0.5 * i * i / v);",
                         std::string("        sum += k * source.Load(int3(clamp(p + int2(") + (horizontal ? "i, 0" : "0, i") + "), int2(0, 0), last), 0));",
                         "        total += k;",
                         "    }",
                         "    return sum / total;", "}"});
        }

        std::string GlowCombineHlsl()
        {
            return Join({"Texture2D<float4> level : register(t0);", "SamplerState level_sampler : register(s0);", "",
                         "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0", "{",
                         "    return float4(level.SampleLevel(level_sampler, uv, 0).rgb, 1.0);", "}"});
        }

        // stock bloom_apply (pimp_shader_hdr_5b71154) without T6's bloom term: x = sqrt(4 s^2), the highlight
        // shoulder, then the 32^3 LUT (1024 x 32, blue slices side by side). With WaW glow, the LUT alpha holds
        // the active vision's glow intensity (GLOW_UNITS; hdr_create_lut2dv passes the atlas alpha
        // through, blending visions like the colour). WaW films the frame, then screen-blends the glow
        // (srcBlend invdestcolor, dstBlend one, src = bloom * glowApply.w, sub_733B80); the overlay films after
        // this pass, so the frame written is the one the overlay films into that result.
        std::string ApplyHlsl(const std::optional<Json>& film, const double glowRange)
        {
            std::vector<std::string> lines{"Texture2D<float4> scene : register(t0);", "Texture2D<float4> glow : register(t1);",
                                           "Texture2D<float4> lut : register(t2);", "SamplerState glow_sampler : register(s0);",
                                           "SamplerState lut_sampler : register(s1);", "SamplerState scene_sampler : register(s2);"};
            if (film)
            {
                for (auto& l : FilmHlsl(*film))
                    lines.push_back(std::move(l));
                for (const char* l : {"", "float3 unfilm(float3 t)", "{", "    float3 y = t;", "    [unroll] for (int i = 0; i < 4; i++)", "    {",
                                      "        float3 f = film(y);", "        float3 d = (film(y + 0.004) - f) / 0.004;",
                                      "        y += (t - f) / (abs(d) > 0.001 ? d : 1.0);", "    }", "    return y;", "}"})
                    lines.emplace_back(l);
            }
            for (const auto& l : std::vector<std::string>{
                     "", "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0", "{",
                     "    float3 s = scene.Sample(scene_sampler, uv).rgb;",
                     "    float3 x = sqrt(s * s * 4.0);",
                     "    float3 knee = (1.0 - exp(x * -" + F(SHOULDER1) + " + " + F(SHOULDER0) + ")) * 0.25 + 0.75;",
                     "    x = " + F(SHOULDER_KNEE) + " < x ? knee : x;",
                     "    float blue = x.b * 31.0;",
                     "    float slice = floor(blue);",
                     "    float u = slice * 32.0 + x.r * 31.0 + 0.5;",
                     "    float v = x.g * 0.96875 + 0.015625;",
                     "    float4 a = lut.Sample(lut_sampler, float2(u / 1024.0, v));",
                     "    float4 b = lut.Sample(lut_sampler, float2((u + 32.0) / 1024.0, v));",
                     "    float4 c = a + frac(blue) * (b - a);"})
                lines.push_back(l);
            if (film)
                for (const auto& l : std::vector<std::string>{
                         "    float code = 0.0;",
                         "    [unroll] for (int i = 0; i < " + std::to_string(GLOW_TEXELS_X / 2) + "; i++)",
                         "        code += lut.SampleLevel(lut_sampler, float2((2.0 * i + 1.0) / 1024.0, 1.0 / 32.0), 0).a;",
                         "    float intensity = code / " + std::to_string(GLOW_TEXELS_X / 2) + ".0 * " + F(glowRange) + ";",
                         "    float3 g = saturate(intensity * glow.Sample(glow_sampler, uv).rgb);",
                         "    if (any(g > 0.0))", "    {", "        float3 f = saturate(film(c.rgb));",
                         "        c.rgb = saturate(unfilm(f + g * (1.0 - f)));", "    }"})
                    lines.push_back(l);
            lines.push_back("    return float4(c.rgb, dot(c.rgb, " + F3({LUMA[0], LUMA[1], LUMA[2]}) + "));");
            lines.emplace_back("}");
            return Join(lines);
        }

        // a stock code material with its pixel shader replaced, written under the stock name for the mod.ff
        // shader relink (ActivateModShaders); BO2's own linker keeps the stock material in the baseline link
        void OverrideCodeMaterial(const fs::path& project, const fs::path& techsetDump, const std::string& name, const std::string& hlsl,
                                  const std::vector<shaders::T6PassArg>& extra)
        {
            auto material = materials::T6MaterialFromJson(ReadJson(techsetDump / "materials" / files::FromUtf8(name + ".json")));
            auto technique = shaders::ReadT6TechniqueSet(techsetDump / "techniquesets" / files::FromUtf8(material.techniqueSet + ".json"));
            const auto code = shaders::CompileHlsl(hlsl, "ps_5_0");
            const auto shader = "waw/glow_" + hash::Sha256Hex(code).substr(0, 20);
            const auto target = project / "shader_bin" / files::FromUtf8("ps_" + shader + ".cso");
            fs::create_directories(target.parent_path());
            files::WriteBytes(target, code);
            technique.name = shader + "_" + name;
            for (size_t slot = 0; slot < technique.techniques.size(); slot++)
                if (auto& t = technique.techniques[slot].technique)
                {
                    t->name = technique.name + "_" + std::to_string(slot);
                    for (auto& p : t->passes)
                    {
                        p.pixelShader = shader;
                        p.args.insert(p.args.end(), extra.begin(), extra.end());
                        p.stableArgCount += static_cast<long long>(extra.size());
                    }
                }
            const auto path = project / "techniquesets" / files::FromUtf8(technique.name + ".json");
            fs::create_directories(path.parent_path());
            files::WriteText(path, util::JsonDumps(shaders::ToJson(technique), 2));
            material.techniqueSet = technique.name;
            for (auto& texture : material.textures)
            {
                auto image = texture.image;
                if (StartsWith(image, ","))
                    image.erase(0, 1);
                texture.image = StartsWith(image, "$") ? "," + image : image;
            }
            const auto out = project / CODE_MATERIAL_OVERRIDES / assets::MaterialPath(name);
            fs::create_directories(out.parent_path());
            files::WriteText(out, materials::T6MaterialText(material));
        }

        constexpr long long CODE_TEXTURE_ARG = 4;
        // texture argument location: sampler << 8 | texture
        constexpr long long SLOT_1 = 0x101;

        // glow.stage: one overlay material per vision (plus the identity vision)
        Json StageGlow(const fs::path& project, const Json& grading, const fs::path& techsetDump, const std::map<std::string, size_t>& nakedCalls)
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
            Json report{{"overlays", Json::object()}, {"glow", Json::object()}, {"notes", Json::object()}, {"materials", Json::array()}, {"errors", Json::array()}, {"technique_donor", base->name}};
            std::map<std::string, Json> visions{{"_identity", Json{{"r_filmenable", "0"}}}};
            for (const auto& [name, fields] : grading.items())
                visions[name] = fields;

            // WaW has no bloom besides its glow: bloom_apply always loses T6's bloom term
            report["mod_materials"] = Json::array();
            const std::vector<std::string> chainMaterials{"bloom_apply", "bloom_downsample", "bloom_blur_x", "bloom_blur_y", "bloom_combine_hilo"};
            bool stock = true;
            for (const auto& name : chainMaterials)
                if (!IsFile(techsetDump / "materials" / files::FromUtf8(name + ".json")))
                {
                    report["errors"].push_back("stock T6 code material " + name + " missing from the technique dump: T6 bloom kept, WaW glow not drawn");
                    stock = false;
                }
            // one chain, so one vision's setup and radius: the glowing vision the level starts with (createart),
            // else the one its scripts set most, then the first by name
            std::optional<std::string> chainVision;
            size_t calls = 0;
            for (const auto& [vision, fields] : visions)
                if (Glow(fields))
                {
                    const auto it = nakedCalls.find(vision);
                    const size_t n = it == nakedCalls.end() ? 0 : it->second;
                    if (!chainVision || n > calls)
                        chainVision = vision, calls = n;
                }
            const bool chain = stock && chainVision;
            double glowRange = 0;
            if (stock)
            {
                std::optional<Json> film;
                for (const auto& [vision, fields] : visions)
                    if (const auto glow = Glow(fields))
                        glowRange = std::max(glowRange, glow->intensity);
                if (chain)
                    film = grading.contains(FILM_TWEAK_GRADE) ? grading[FILM_TWEAK_GRADE] : visions[*chainVision];
                OverrideCodeMaterial(project, techsetDump, "bloom_apply", ApplyHlsl(film, glowRange), {});
                report["mod_materials"].push_back("bloom_apply");
            }
            if (chain)
            {
                const auto& fields = visions[*chainVision];
                const auto glow = *Glow(fields);
                const auto& film = grading.contains(FILM_TWEAK_GRADE) ? grading[FILM_TWEAK_GRADE] : fields;
                OverrideCodeMaterial(project, techsetDump, "bloom_downsample", GlowSetupHlsl(film, glow), {});
                // the frame texture only gives the blur the screen size
                OverrideCodeMaterial(project, techsetDump, "bloom_blur_x", GlowBlurHlsl(glow, true), {{0, SLOT_1, 1, CODE_TEXTURE_ARG, SCENE_CODE_TEXTURE}});
                OverrideCodeMaterial(project, techsetDump, "bloom_blur_y", GlowBlurHlsl(glow, false), {{0, SLOT_1, 1, CODE_TEXTURE_ARG, SCENE_CODE_TEXTURE}});
                OverrideCodeMaterial(project, techsetDump, "bloom_combine_hilo", GlowCombineHlsl(), {});
                for (const auto& name : chainMaterials)
                    if (name != "bloom_apply")
                        report["mod_materials"].push_back(name);
                report["glow_chain"] = Json{{"vision", *chainVision}, {"selection_score", calls}, {"range", glowRange}, {"cutoff", glow.cutoff},
                                            {"desaturation", glow.desaturation}, {"radius", glow.radius}};
            }
            for (const auto& [vision, fields] : visions)
            {
                Json notes = Json::array();
                auto glow = Glow(fields);
                if (glow && !chain)
                    glow.reset();
                if (glow)
                {
                    if (Num(fields, "r_glowrayintensity", 0) > 0)
                        notes.push_back("god rays (r_glowRayIntensity) not reproduced");
                    if (glow->intensity < 0)
                        notes.push_back("negative glow intensity " + F(glow->intensity) + " drawn as 0");
                    if (vision != *chainVision)
                        notes.push_back("glow cutoff, desaturation and radius are " + *chainVision + "'s (one T6 bloom chain); intensity is this vision's");
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
                p.args.push_back({0, 0, 1, CODE_TEXTURE_ARG, SCENE_CODE_TEXTURE});
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
                report["glow"][vision] = glow ? Json{{"intensity", glow->intensity}} : Json(nullptr);
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
        {
            // alpha: the glow intensity when the glow chain reads it, else opaque
            const int units = grade.contains("glow_units") ? grade["glow_units"].get<int>() : -1;
            for (int green = 0; green < 32; green++)
                for (int blue = 0; blue < 32; blue++)
                    for (int red = 0; red < 32; red++)
                    {
                        const auto rgb = Film({table[red], table[green], table[blue]}, grade);
                        for (const double x : rgb)
                            data += static_cast<char>(static_cast<int>(std::nearbyint(x * 255)));
                        int alpha = 255;
                        if (units >= 0 && green < 2 && blue == 0 && red < GLOW_TEXELS_X)
                            alpha = std::clamp(units - 3 * (green * GLOW_TEXELS_X + red), 0, 3) * 85;
                        data += static_cast<char>(alpha);
                    }
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
        {
            // the code materials of an earlier build of this stage
            std::error_code e;
            fs::remove_all(project / CODE_MATERIAL_OVERRIDES, e);
        }
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
        std::map<std::string, size_t> nakedCalls;
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
                {
                    wanted.insert(value);
                    // VisionSetNaked (translated _compat::waw_visionsetnaked) and the utility that sets it for every
                    // player; the level's createart script (maps/createart/<map>_art) sets the starting vision
                    const auto api = Lower(tokens[i - 2].text);
                    if (api == "visionsetnaked" || api == "waw_visionsetnaked" || api.ends_with("set_all_players_visionset"))
                        nakedCalls[value] += Lower(files::Utf8(p.parent_path().filename())) == "createart" ? CREATEART_WEIGHT : 1;
                }
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
            const auto overlays = StageGlow(project, grading, *techsetDump, nakedCalls);
            init.emplace_back("    level.waw2bo2_vision_overlays = [];");
            for (const auto& [name, material] : overlays["overlays"].items())
            {
                init.push_back("    level.waw2bo2_vision_overlays[\"" + name + "\"] = \"" + material.get<std::string>() + "\";");
                init.push_back("    precacheshader( \"" + material.get<std::string>() + "\" );");
            }
            report["overlay"] = overlays;
            for (const auto& name : overlays["mod_materials"])
                zoneLines += "material," + name.get<std::string>() + "\n";
            for (const auto& error : overlays["errors"])
                report["errors"].push_back(error);
            // film now runs exactly in the overlay; the LUT keeps the shoulder inverse and, with the glow
            // chain, carries each vision's glow intensity in its alpha
            const bool chain = overlays.contains("glow_chain");
            size_t index = 0;
            std::vector<std::string> order{"_identity"};
            for (const auto& [name, _] : report["visions"].items())
                order.push_back(name);
            for (auto& g : grades)
            {
                g = Json{{"r_filmenable", "0"}};
                if (chain)
                {
                    const auto& entry = overlays["glow"].value(order[index], Json());
                {
                    const double range = overlays["glow_chain"]["range"].get<double>();
                    const double intensity = entry.is_object() ? entry["intensity"].get<double>() : 0.;
                    g["glow_units"] = static_cast<int>(std::nearbyint(std::clamp(intensity / range, 0., 1.) * GLOW_UNITS));
                }
                }
                index++;
            }
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
