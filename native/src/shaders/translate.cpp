// Port of shaders._translate (src/waw2bo2/shaders.py). Every branch, message
// and output line follows the Python; the tests compare both on real shaders.
#include "translate.h"

#include "d3d.h"
#include "registers.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::shaders
{
    namespace
    {
        using py::Json;

        template<typename T> struct Ordered // a Python dict: insertion order, assignment keeps position
        {
            std::vector<std::pair<std::string, T>> items;

            T* Find(const std::string& key)
            {
                for (auto& [k, v] : items)
                    if (k == key)
                        return &v;
                return nullptr;
            }
            const T* Find(const std::string& key) const
            {
                for (const auto& [k, v] : items)
                    if (k == key)
                        return &v;
                return nullptr;
            }
            bool Has(const std::string& key) const
            {
                return Find(key) != nullptr;
            }
            void Set(const std::string& key, T value)
            {
                if (auto* found = Find(key))
                    *found = std::move(value);
                else
                    items.emplace_back(key, std::move(value));
            }
        };

        struct Interface
        {
            std::string semantic;
            long long width = 0;
        };

        // dict.get on a JSON object (nullptr when absent, null or not an object)
        const Json* Get(const Json* object, const char* key)
        {
            if (!object || !object->is_object())
                return nullptr;
            const auto it = object->find(key);
            if (it == object->end() || it->is_null())
                return nullptr;
            return &*it;
        }

        const Json* Get(const Json* object, const std::string& key)
        {
            return Get(object, key.c_str());
        }

        bool Truthy(const Json* value)
        {
            if (!value || value->is_null())
                return false;
            if (value->is_boolean())
                return value->get<bool>();
            if (value->is_number_integer() || value->is_number_unsigned())
                return value->get<long long>() != 0;
            if (value->is_number_float())
                return value->get<double>() != 0.0;
            if (value->is_string()) // json::empty() is false for every string
                return !value->get_ref<const std::string&>().empty();
            if (value->is_array() || value->is_object())
                return !value->empty();
            return true;
        }

        // type(x) is int (JSON bools are not ints)
        bool IsInt(const Json* value)
        {
            return value && (value->is_number_integer() || value->is_number_unsigned());
        }

        // str(x) inside an f-string
        std::string Str(const Json& value)
        {
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_boolean())
                return value.get<bool>() ? "True" : "False";
            if (value.is_number_float())
                return py::FloatRepr(value.get<double>());
            if (value.is_number_integer() || value.is_number_unsigned())
                return std::to_string(value.get<long long>());
            return value.dump();
        }

        // repr(float(x))
        std::string FloatOf(const Json& value)
        {
            return py::FloatRepr(value.get<double>());
        }

        std::string Upper(std::string text)
        {
            for (auto& c : text)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return text;
        }

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        std::string Join(const std::vector<std::string>& parts, const std::string& sep)
        {
            std::string out;
            for (size_t i = 0; i < parts.size(); i++)
            {
                if (i)
                    out += sep;
                out += parts[i];
            }
            return out;
        }

        bool StartsWith(const std::string_view text, const std::string_view prefix)
        {
            return text.substr(0, prefix.size()) == prefix;
        }

        bool EndsWith(const std::string_view text, const std::string_view suffix)
        {
            return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
        }

        // re.fullmatch(prefix + r'\d+', text)
        bool PrefixDigits(const std::string_view text, const std::string_view prefix)
        {
            if (!StartsWith(text, prefix) || text.size() == prefix.size())
                return false;
            return std::all_of(text.begin() + static_cast<long long>(prefix.size()), text.end(), py::IsDigit);
        }

        std::string Repeat(const std::string& text, const long long count)
        {
            std::string out;
            for (long long i = 0; i < count; i++)
                out += text;
            return out;
        }

        struct Operation
        {
            std::string op;
            std::vector<std::string> args;
            std::string line;
        };

        struct Translator
        {
            const Json* bindings; // the bindings dict (nullptr: {})
            bool bound;

            std::string stage;
            Ordered<std::vector<double>> defs;
            Ordered<std::pair<long long, bool>> integers; // first value, overflowed
            Ordered<Interface> inputs, outputs;
            Ordered<std::string> samplers;
            std::vector<Operation> operations;
            std::set<std::string> used, centroid;
            std::vector<std::string> writes;

            std::map<long long, long long> cbSizes;
            Ordered<std::string> regExpr;
            Ordered<std::string> sampleScale, sampleAlpha, sampleTexel;

            const Json* Section(const char* name) const
            {
                return Get(bindings, name);
            }

            void UseBuffers(const Json* uses)
            {
                if (!uses)
                    return;
                for (const auto& use : *uses)
                {
                    const Json* slot = use.size() > 0 ? &use[0] : nullptr;
                    const Json* index = use.size() > 1 ? &use[1] : nullptr;
                    if (use.size() != 2 || !IsInt(slot) || !IsInt(index))
                        throw ShaderError("invalid adapter buffer use");
                    const auto s = slot->get<long long>();
                    const auto i = index->get<long long>();
                    if (!(0 <= s && s < 14) || !(0 <= i && i < 4096))
                        throw ShaderError("invalid adapter buffer use");
                    cbSizes[s] = std::max(cbSizes.count(s) ? cbSizes[s] : 0, i + 1);
                }
            }

            void Parse(const std::string_view assembly)
            {
                std::vector<std::string> lines;
                for (const auto raw : py::SplitLines(assembly))
                {
                    const auto cut = raw.find("//");
                    const auto line = py::RStrip(py::Strip(raw.substr(0, cut)), std::string_view("\0", 1));
                    if (!line.empty())
                        lines.emplace_back(line);
                }
                if (lines.empty() || (lines[0] != "ps_3_0" && lines[0] != "vs_3_0"))
                    throw ShaderError("requires ps_3_0 or vs_3_0 bytecode");
                stage = lines[0].substr(0, 2);
                lines.erase(lines.begin());

                for (const auto& line : lines)
                {
                    const auto space = line.find(' ');
                    const std::string op = line.substr(0, space);
                    const std::string tail = space == std::string::npos ? "" : line.substr(space + 1);
                    std::vector<std::string> args;
                    for (const auto part : py::Split(tail, ","))
                        args.emplace_back(py::Strip(part));

                    if (op == "def")
                    {
                        if (args.size() != 5 || !PrefixDigits(args[0], "c"))
                            throw ShaderError("invalid constant declaration: " + line);
                        std::vector<double> values;
                        for (size_t i = 1; i < 5; i++)
                        {
                            double value = 0;
                            if (!py::ParseFloat(args[i], value))
                                throw ShaderError("invalid literal: " + line);
                            values.push_back(value);
                        }
                        if (!std::all_of(values.begin(), values.end(), [](const double v) { return std::isfinite(v); }))
                            throw ShaderError("nonfinite literal: " + line);
                        defs.Set(args[0], values);
                    }
                    else if (op == "defi")
                    {
                        if (args.size() != 5 || !PrefixDigits(args[0], "i"))
                            throw ShaderError("invalid integer declaration: " + line);
                        std::pair<long long, bool> first{0, false};
                        for (size_t i = 1; i < 5; i++)
                        {
                            long long value = 0;
                            bool overflow = false;
                            if (!py::ParseInt(args[i], value, overflow))
                                throw ShaderError("invalid integer literal: " + line);
                            if (i == 1)
                                first = {value, overflow};
                        }
                        integers.Set(args[0], first);
                    }
                    else if (StartsWith(op, "dcl_"))
                    {
                        Register match;
                        const bool isRegister = MatchRegister(args[0], match);
                        if (PrefixDigits(args[0], "s"))
                        {
                            const auto dim = op.substr(4);
                            if (dim != "2d" && dim != "cube" && dim != "volume")
                                throw ShaderError("unsupported texture declaration: " + line);
                            samplers.Set(args[0], dim);
                        }
                        else if (isRegister)
                        {
                            std::vector<std::string> declaration;
                            for (const auto part : py::Split(std::string_view(op).substr(4), "_"))
                                declaration.emplace_back(part);
                            for (size_t i = 1; i < declaration.size(); i++)
                                if (declaration[i] != "pp" && declaration[i] != "centroid")
                                    throw ShaderError("unsupported declaration modifier: " + line);
                            std::string semantic = Upper(declaration[0]);
                            if (std::find(declaration.begin() + 1, declaration.end(), "centroid") != declaration.end())
                                centroid.insert(match.reg);
                            if (StartsWith(semantic, "POSITION") && StartsWith(match.reg, "o"))
                                semantic = "SV_Position";
                            else if (semantic.empty() || !py::IsDigit(semantic.back()))
                                semantic += "0";
                            const std::string mask = SwizzleXyzw(match.hasSwizzle ? match.swizzle : "xyzw");
                            long long width = 0;
                            for (const char c : mask)
                                width = std::max<long long>(width, static_cast<long long>(std::string_view("xyzw").find(c)) + 1);
                            (StartsWith(match.reg, "o") ? outputs : inputs).Set(match.reg, {semantic, width});
                        }
                        else
                            throw ShaderError("unsupported declaration: " + line);
                    }
                    else
                    {
                        operations.push_back({op, args, line});
                        for (auto& reg : FindRegisters(tail))
                            used.insert(std::move(reg));
                        Register dst;
                        if (!args.empty() && MatchRegister(args[0], dst)
                            && std::find(writes.begin(), writes.end(), dst.reg) == writes.end())
                            writes.push_back(dst.reg);
                    }
                }

                for (const auto& reg : writes)
                {
                    if (StartsWith(reg, "o") && !outputs.Has(reg))
                    {
                        if (stage == "ps" && PrefixDigits(reg, "oC"))
                            outputs.Set(reg, {"SV_Target" + reg.substr(2), 4});
                        else if (reg == "oDepth")
                            outputs.Set(reg, {"SV_Depth", 1});
                        else
                            throw ShaderError("missing output declaration for " + reg);
                    }
                }
                if (outputs.items.empty())
                    throw ShaderError("shader has no output");
            }

            std::vector<std::string> Constants() const
            {
                std::vector<std::string> constants;
                for (const auto& reg : used)
                    if (!defs.Has(reg) && StartsWith(reg, "c"))
                        constants.push_back(reg);
                std::sort(constants.begin(), constants.end());
                return constants;
            }

            Ordered<Interface> Bind(const char* kind, const Ordered<Interface>& declarations)
            {
                Ordered<Interface> result;
                const Json* section = Section(kind);
                for (const auto& [reg, declared] : declarations.items)
                {
                    const Json* entry = Get(section, reg);
                    if (bound && !entry)
                        throw ShaderError(std::string("missing target ") + kind + " binding " + reg);
                    std::string semantic = declared.semantic;
                    long long width = declared.width;
                    bool widthIsInt = true;
                    if (Truthy(entry))
                    {
                        const Json& sem = entry->at("semantic");
                        semantic = sem.is_string() ? sem.get<std::string>() : Str(sem);
                        if (const Json* w = Get(entry, "width"))
                        {
                            widthIsInt = IsInt(w);
                            width = widthIsInt ? w->get<long long>() : 0;
                        }
                    }
                    if (!MatchSemantic(Upper(semantic)) || !widthIsInt || !(1 <= width && width <= 4))
                        throw ShaderError("invalid interface for " + reg);
                    const Json* adapterJson = Truthy(entry) ? Get(entry, "adapter") : nullptr;
                    const std::string adapter = adapterJson && adapterJson->is_string() ? adapterJson->get<std::string>() : "";
                    const bool hasAdapter = adapterJson != nullptr;
                    if (hasAdapter && adapter == "waw_neutral" && std::string_view(kind) == "inputs")
                    {
                        const Json* constant = Get(entry, "constant");
                        if (constant && constant->size() == 4)
                        {
                            result.Set(reg, {semantic, width});
                            continue;
                        }
                    }
                    if (hasAdapter
                        && !(std::string_view(kind) == "inputs" && declared.width == 4
                             && ((adapter == "waw_half_uv" && width == 2) || (adapter == "waw_position" && width == 3)
                                 || (adapter == "waw_ubyte4_vector" && width == 3) || ((adapter == "waw_uv_lightmap" || adapter == "waw_uv") && width == 2))))
                        throw ShaderError("unsupported vertex input adapter for " + reg);
                    if (width < declared.width && !hasAdapter)
                        throw ShaderError("target interface truncates source channels for " + reg);
                    result.Set(reg, {semantic, width});
                }
                return result;
            }

            std::string Source(const std::string& token)
            {
                if (StartsWith(token, "-"))
                    return "(-" + Source(token.substr(1)) + ")";
                if (StartsWith(token, "abs(") && EndsWith(token, ")") && token.size() >= 5)
                    return "abs(" + Source(token.substr(4, token.size() - 5)) + ")";
                std::string base, swizzle;
                if (MatchAbsModifier(token, base, swizzle))
                    return "abs(" + Source(base + swizzle) + ")";
                Register match;
                if (!MatchRegister(token, match))
                    throw ShaderError("unsupported operand " + token);
                if (StartsWith(match.reg, "v") && !inputs.Has(match.reg))
                    throw ShaderError("undeclared input " + match.reg);
                const auto* mapped = regExpr.Find(match.reg);
                std::string expr = mapped ? *mapped : match.reg;
                if (match.hasSwizzle)
                {
                    std::string swz = SwizzleXyzw(match.swizzle);
                    swz += std::string(4 - swz.size(), swz.back());
                    expr = "(" + expr + ")." + swz;
                }
                return expr;
            }

            Translation Run(std::string_view assembly);
        };

        std::string Fields(const Ordered<Interface>& items, const std::set<std::string>& centroid)
        {
            std::vector<std::string> parts;
            for (const auto& [reg, decl] : items.items)
                parts.push_back((centroid.count(reg) ? "centroid " : "") + std::string("float") + (decl.width > 1 ? std::to_string(decl.width) : "")
                                + " " + reg + " : " + decl.semantic + ";");
            return Join(parts, " ");
        }

        Translation Translator::Run(const std::string_view assembly)
        {
            Parse(assembly);
            const auto constants = Constants();
            std::vector<std::string> resources;

            const Json* constantBindings = Section("constants");
            for (const auto& reg : constants)
            {
                const Json* entry = Get(constantBindings, reg);
                if (bound && !entry)
                    throw ShaderError("missing target constant binding " + reg);
                if (Truthy(entry) && entry->contains("expr"))
                {
                    UseBuffers(Get(entry, "uses"));
                    regExpr.Set(reg, "(" + Str(entry->at("expr")) + ")");
                    continue;
                }
                long long slot = 0, index = std::stoll(reg.substr(1));
                if (Truthy(entry))
                {
                    const Json* s = Get(entry, "buffer");
                    const Json* i = Get(entry, "index");
                    if (!IsInt(s) || !IsInt(i))
                        throw ShaderError("invalid constant binding " + reg);
                    slot = s->get<long long>();
                    index = i->get<long long>();
                }
                if (!(0 <= slot && slot < 14) || !(0 <= index && index < 4096))
                    throw ShaderError("invalid constant binding " + reg);
                cbSizes[slot] = std::max(cbSizes.count(slot) ? cbSizes[slot] : 0, index + 1);
                regExpr.Set(reg, "t6_cb" + std::to_string(slot) + "[" + std::to_string(index) + "]");
            }

            const Json* samplerBindings = Section("samplers");
            if (samplerBindings && samplerBindings->is_object())
            {
                for (const auto& [reg, entry] : samplerBindings->items())
                {
                    if (Truthy(Get(&entry, "rgb_scale")) || Truthy(Get(&entry, "alpha_expr")))
                        UseBuffers(Get(&entry, "uses"));
                    if (Truthy(Get(&entry, "rgb_scale")))
                        sampleScale.Set(reg, Str(entry.at("rgb_scale")));
                    if (Truthy(Get(&entry, "alpha_expr")))
                        sampleAlpha.Set(reg, Str(entry.at("alpha_expr")));
                    if (Truthy(Get(&entry, "texel_expr")))
                    {
                        UseBuffers(Get(&entry, "uses"));
                        sampleTexel.Set(reg, Str(entry.at("texel_expr")));
                    }
                }
            }
            const Json* epilogue = Section("epilogue");
            if (Truthy(epilogue))
                UseBuffers(Get(epilogue, "uses"));
            for (const auto& [slot, count] : cbSizes)
                resources.push_back("cbuffer WawBuffer" + std::to_string(slot) + " : register(b" + std::to_string(slot) + ") { float4 t6_cb"
                                    + std::to_string(slot) + "[" + std::to_string(count) + "]; };");
            for (const auto& [reg, values] : defs.items)
            {
                std::vector<std::string> text;
                for (const auto v : values)
                    text.push_back(py::FloatRepr(v));
                resources.push_back("static const float4 " + reg + " = float4(" + Join(text, ", ") + ");");
            }

            std::vector<std::pair<std::string, std::string>> declared; // (typ|tex|samp) -> reg
            for (const auto& [reg, dim] : samplers.items)
            {
                const Json* entry = Get(samplerBindings, reg);
                if (bound && !entry)
                    throw ShaderError("missing target sampler binding " + reg);
                long long tex = std::stoll(reg.substr(1)), samp = tex;
                if (Truthy(entry))
                {
                    const Json* t = Get(entry, "texture");
                    const Json* s = Get(entry, "sampler");
                    if (!IsInt(t) || !IsInt(s))
                        throw ShaderError("invalid resource binding " + reg);
                    tex = t->get<long long>();
                    samp = s->get<long long>();
                }
                if (!(0 <= tex && tex < 128) || !(0 <= samp && samp < 16))
                    throw ShaderError("invalid resource binding " + reg);
                const std::string typ = dim == "2d" ? "Texture2D" : dim == "cube" ? "TextureCube" : "Texture3D";
                if (Truthy(entry) && Get(entry, "constant"))
                    continue;
                if (Truthy(entry) && Truthy(Get(entry, "point_load")))
                {
                    if (dim != "2d")
                        throw ShaderError("point load adapter needs a 2D texture: " + reg);
                    resources.push_back(typ + "<float4> tex_" + reg + " : register(t" + std::to_string(tex) + ");");
                    resources.push_back("float4 load_" + reg + "(float2 uv) { uint w, h; tex_" + reg
                                        + ".GetDimensions(w, h); int2 p = clamp(int2(floor(uv * float2(w, h))), int2(0, 0), int2(w, h) - 1); return tex_" + reg
                                        + ".Load(int3(p, 0)); }");
                    continue;
                }
                const std::string key = typ + "|" + std::to_string(tex) + "|" + std::to_string(samp);
                const auto previous = std::find_if(declared.begin(), declared.end(), [&](const auto& d) { return d.first == key; });
                if (previous != declared.end())
                {
                    resources.push_back("#define tex_" + reg + " tex_" + previous->second);
                    resources.push_back("#define samp_" + reg + " samp_" + previous->second);
                    continue;
                }
                declared.emplace_back(key, reg);
                resources.push_back(typ + "<float4> tex_" + reg + " : register(t" + std::to_string(tex) + ");");
                resources.push_back("SamplerState samp_" + reg + " : register(s" + std::to_string(samp) + ");");
            }
            for (const auto& [reg, code] : sampleTexel.items)
                resources.push_back("float4 waw_texel_" + reg + "(float4 T) { return " + code + "; }");

            inputs = Bind("inputs", inputs);
            outputs = Bind("outputs", outputs);
            if (stage == "vs"
                && std::none_of(outputs.items.begin(), outputs.items.end(), [](const auto& o) { return Lower(o.second.semantic) == "sv_position"; }))
                throw ShaderError("vertex shader needs an explicitly mapped SV_Position output");

            std::vector<std::string> body, flow;
            int repeatIndex = 0;
            const std::map<std::string, std::string> comparison{{"gt", ">"}, {"lt", "<"}, {"ge", ">="}, {"le", "<="}, {"eq", "=="}, {"ne", "!="}};
            static const std::map<std::string, size_t> arities{
                {"mov", 1},  {"add", 2},   {"sub", 2},   {"mul", 2},    {"mad", 3},    {"dp3", 2},    {"dp4", 2},    {"dp2add", 3},
                {"min", 2},  {"max", 2},   {"rcp", 1},   {"rsq", 1},    {"abs", 1},    {"frc", 1},    {"exp", 1},    {"log", 1},
                {"pow", 2},  {"lrp", 3},   {"cmp", 3},   {"slt", 2},    {"sge", 2},    {"nrm", 1},    {"dsx", 1},    {"dsy", 1},
                {"sincos", 1}, {"texld", 2}, {"texldp", 2}, {"texldb", 2}, {"texldl", 2}, {"texldd", 4}};

            for (const auto& [decorated, args, original] : operations)
            {
                std::vector<std::string> flags;
                for (const auto part : py::Split(decorated, "_"))
                    flags.emplace_back(part);
                const std::string op = flags.front();
                flags.erase(flags.begin());
                if (op == "nop")
                    continue;
                if (op == "rep" && flags.empty() && args.size() == 1)
                {
                    std::string counterReg = args[0];
                    if (EndsWith(counterReg, ".x"))
                        counterReg = counterReg.substr(0, counterReg.size() - 2);
                    const auto* integer = PrefixDigits(counterReg, "i") ? integers.Find(counterReg) : nullptr;
                    if (!integer)
                        throw ShaderError("only literal integer repeat counts are supported: " + original);
                    if (integer->second || !(0 <= integer->first && integer->first <= 65535))
                        throw ShaderError("invalid repeat count: " + original);
                    repeatIndex++;
                    const std::string counter = "waw_repeat" + std::to_string(repeatIndex);
                    body.push_back("for (int " + counter + "=0; " + counter + "<" + std::to_string(integer->first) + "; ++" + counter + ") {");
                    flow.emplace_back("rep");
                    continue;
                }
                if (op == "endrep" && flags.empty() && !flow.empty() && flow.back() == "rep")
                {
                    flow.pop_back();
                    body.emplace_back("}");
                    continue;
                }
                if (op == "if" && flags.size() == 1 && comparison.count(flags[0]) && args.size() == 2)
                {
                    body.push_back("if ((" + Source(args[0]) + ").x " + comparison.at(flags[0]) + " (" + Source(args[1]) + ").x) {");
                    flow.emplace_back("if");
                    continue;
                }
                if (op == "else" && flags.empty() && !flow.empty() && flow.back() == "if")
                {
                    flow.back() = "else";
                    body.emplace_back("} else {");
                    continue;
                }
                if (op == "endif" && flags.empty() && !flow.empty() && (flow.back() == "if" || flow.back() == "else"))
                {
                    flow.pop_back();
                    body.emplace_back("}");
                    continue;
                }
                for (const auto& f : flags)
                    if (f != "sat" && f != "pp")
                        throw ShaderError("unsupported instruction modifier: " + original);
                if (op == "texkill" && args.size() == 1 && stage == "ps")
                {
                    body.push_back("clip((" + Source(args[0]) + ").xyz);");
                    continue;
                }
                const auto arity = arities.find(op);
                if (arity == arities.end() || args.size() != arity->second + 1)
                    throw ShaderError("unsupported instruction: " + original);
                Register dst;
                if (!MatchRegister(args[0], dst) || !(StartsWith(dst.reg, "r") || outputs.Has(dst.reg)))
                    throw ShaderError("invalid destination: " + original);
                const std::string reg = dst.reg;
                const std::string mask = SwizzleXyzw(dst.hasSwizzle ? dst.swizzle : "xyzw");
                if (std::set<char>(mask.begin(), mask.end()).size() != mask.size())
                    throw ShaderError("invalid write mask: " + original);
                if (op == "sincos" && mask.find_first_not_of("xy") != std::string::npos)
                    throw ShaderError("sincos writes outside .xy: " + original);

                std::string expr;
                if (StartsWith(op, "texld"))
                {
                    const std::string coord = Source(args[1]);
                    const std::string& sampler = args[2];
                    const auto* dim = samplers.Find(sampler);
                    if (!dim)
                        throw ShaderError("undeclared sampler: " + original);
                    const std::string dims = *dim == "2d" ? "xy" : "xyz";
                    std::string uv = "(" + coord + ")." + dims;
                    const Json* entry = Get(samplerBindings, sampler);
                    const Json* uRemap = Get(entry, "u_scale_offset");
                    const Json* vRemap = Get(entry, "v_scale_offset");
                    if (uRemap)
                    {
                        if (*dim != "2d" || (op != "texld" && op != "texldl"))
                            throw ShaderError("u remap needs a 2D texld/texldl: " + original);
                        uv = "float2((" + uv + ").x * " + FloatOf((*uRemap)[0]) + " + " + FloatOf((*uRemap)[1]) + ", (" + uv + ").y)";
                    }
                    if (vRemap)
                    {
                        if (*dim != "2d" || (op != "texld" && op != "texldl"))
                            throw ShaderError("v remap needs a 2D texld/texldl: " + original);
                        uv = "float2((" + uv + ").x, (" + uv + ").y * " + FloatOf((*vRemap)[0]) + " + " + FloatOf((*vRemap)[1]) + ")";
                    }
                    const bool pointLoad = Truthy(Get(entry, "point_load"));
                    if (pointLoad && op != "texld" && op != "texldl" && op != "texldp")
                        throw ShaderError("point load adapter cannot express " + op + ": " + original);
                    if (op == "texldp")
                        uv = "(" + uv + " / (" + coord + ").w)";
                    const Json* constant = Get(entry, "constant");
                    const std::string sample = "tex_" + sampler;
                    const std::string state = "samp_" + sampler;
                    if (op == "texldl")
                        expr = sample + ".SampleLevel(" + state + ", " + uv + ", (" + coord + ").w)";
                    else if (op == "texldb")
                        expr = sample + ".SampleBias(" + state + ", " + uv + ", (" + coord + ").w)";
                    else if (op == "texldd")
                        expr = sample + ".SampleGrad(" + state + ", " + uv + ", (" + Source(args[3]) + ")." + dims + ", (" + Source(args[4]) + ")." + dims + ")";
                    else
                    {
                        if (stage != "ps")
                            throw ShaderError("vertex texture reads must use explicit LOD");
                        expr = sample + ".Sample(" + state + ", " + uv + ")";
                    }
                    if (pointLoad)
                    {
                        expr = "load_" + sampler + "(" + uv + ")";
                        if (op == "texldp")
                            expr = "(" + expr + " * (" + coord + ").w)";
                        if (Truthy(Get(entry, "reverse_depth_compare")))
                            expr = "float4(2.0 * (" + coord + ").z - (" + expr + ").x, (" + expr + ").yzw)";
                    }
                    if (constant)
                    {
                        std::vector<std::string> values;
                        for (const auto& c : *constant)
                            values.push_back(FloatOf(c));
                        expr = "float4(" + Join(values, ", ") + ")";
                    }
                    if (const auto* scale = sampleScale.Find(sampler))
                        expr = "(" + expr + " * float4((" + *scale + ").xxx, 1.0))";
                    if (sampleTexel.Has(sampler))
                        expr = "(waw_texel_" + sampler + "(" + expr + "))";
                    if (const auto* alpha = sampleAlpha.Find(sampler))
                        expr = "float4((" + expr + ").xyz, (" + *alpha + "))";
                }
                else
                {
                    std::vector<std::string> v;
                    for (size_t i = 1; i < args.size(); i++)
                        v.push_back(Source(args[i]));
                    const std::string& a = v[0];
                    const std::string b = v.size() > 1 ? v[1] : "";
                    const std::string c = v.size() > 2 ? v[2] : "";
                    if (op == "mov")
                        expr = a;
                    else if (op == "add")
                        expr = "(" + a + "+" + b + ")";
                    else if (op == "sub")
                        expr = "(" + a + "-" + b + ")";
                    else if (op == "mul")
                        expr = "(" + a + "*" + b + ")";
                    else if (op == "mad")
                        expr = "(" + a + "*" + b + "+" + c + ")";
                    else if (op == "dp3")
                        expr = "dot((" + a + ").xyz, (" + b + ").xyz)";
                    else if (op == "dp4")
                        expr = "dot(" + a + ", " + b + ")";
                    else if (op == "dp2add")
                        expr = "(dot((" + a + ").xy, (" + b + ").xy) + (" + c + ").x)";
                    else if (op == "min")
                        expr = "min(" + a + "," + b + ")";
                    else if (op == "max")
                        expr = "max(" + a + "," + b + ")";
                    else if (op == "rcp")
                        expr = "(1.0 / (" + a + ").x)";
                    else if (op == "rsq")
                        expr = "rsqrt(abs((" + a + ").x))";
                    else if (op == "abs")
                        expr = "abs(" + a + ")";
                    else if (op == "frc")
                        expr = "frac(" + a + ")";
                    else if (op == "exp")
                        expr = "exp2((" + a + ").x)";
                    else if (op == "log")
                        expr = "log2(abs((" + a + ").x))";
                    else if (op == "pow")
                        expr = "pow(abs((" + a + ").x), (" + b + ").x)";
                    else if (op == "lrp")
                        expr = "(" + a + "*" + b + "+(1.0-" + a + ")*" + c + ")";
                    else if (op == "cmp")
                        expr = "((" + a + ">=0.0)?" + b + ":" + c + ")";
                    else if (op == "slt")
                        expr = "((" + a + "<" + b + ")?1.0:0.0)";
                    else if (op == "sge")
                        expr = "((" + a + ">=" + b + ")?1.0:0.0)";
                    else if (op == "nrm")
                        expr = "float4(normalize((" + a + ").xyz), (" + a + ").w)";
                    else if (op == "dsx")
                        expr = "ddx(" + a + ")";
                    else if (op == "dsy")
                        expr = "ddy(" + a + ")";
                    else if (op == "sincos")
                        expr = "float4(cos((" + a + ").x), sin((" + a + ").x), 0.0, 0.0)";
                }
                if (std::find(flags.begin(), flags.end(), "sat") != flags.end())
                    expr = "saturate(" + expr + ")";
                body.push_back("{ float4 value = " + expr + "; " + reg + "." + mask + " = value." + mask + "; }");
            }
            if (!flow.empty())
                throw ShaderError("unclosed flow control");

            std::set<std::string> all(used);
            for (const auto& [reg, _] : inputs.items)
                all.insert(reg);
            for (const auto& [reg, _] : outputs.items)
                all.insert(reg);
            std::vector<std::string> decls;
            for (const auto& reg : all)
                if (StartsWith(reg, "r") || StartsWith(reg, "v") || StartsWith(reg, "o"))
                    decls.push_back("float4 " + reg + " = 0;");

            const Json* inputBindings = Section("inputs");
            const auto adapterOf = [&](const std::string& reg) -> std::string
            {
                const Json* entry = Get(inputBindings, reg);
                const Json* adapter = Get(entry, "adapter");
                return adapter && adapter->is_string() ? adapter->get<std::string>() : "";
            };

            const Json* signature = Section("input_signature");
            Ordered<std::string> fieldOf;
            for (const auto& [reg, _] : inputs.items)
                fieldOf.Set(reg, reg);
            std::string inputFields;
            if (signature)
            {
                std::vector<std::pair<std::string, size_t>> bySemantic; // later entries win
                for (size_t i = 0; i < signature->size(); i++)
                {
                    const auto semantic = Upper((*signature)[i].at("semantic").get<std::string>());
                    const auto it = std::find_if(bySemantic.begin(), bySemantic.end(), [&](const auto& e) { return e.first == semantic; });
                    if (it != bySemantic.end())
                        it->second = i;
                    else
                        bySemantic.emplace_back(semantic, i);
                }
                for (const auto& [reg, decl] : inputs.items)
                {
                    const auto semantic = Upper(decl.semantic);
                    const auto it = std::find_if(bySemantic.begin(), bySemantic.end(), [&](const auto& e) { return e.first == semantic; });
                    if (it == bySemantic.end() || (*signature)[it->second].at("width").get<long long>() < decl.width)
                        throw ShaderError("paired signature cannot supply " + reg + " " + decl.semantic + "/" + std::to_string(decl.width));
                    fieldOf.Set(reg, "link" + std::to_string(it->second));
                }
                std::set<std::string> interpolated;
                for (const auto& reg : centroid)
                    if (const auto* field = fieldOf.Find(reg))
                        interpolated.insert(*field);
                std::vector<std::string> parts;
                for (size_t i = 0; i < signature->size(); i++)
                {
                    const auto& s = (*signature)[i];
                    const auto width = s.at("width").get<long long>();
                    const std::string link = "link" + std::to_string(i);
                    parts.push_back((interpolated.count(link) ? "centroid " : "") + std::string("float") + (width > 1 ? std::to_string(width) : "") + " "
                                    + link + " : " + Str(s.at("semantic")) + ";");
                }
                inputFields = Join(parts, " ");
            }
            else
            {
                Ordered<Interface> visible;
                for (const auto& [reg, decl] : inputs.items)
                    if (adapterOf(reg) != "waw_neutral")
                        visible.Set(reg, decl);
                inputFields = Fields(visible, centroid);
                for (const auto& [reg, _] : inputs.items)
                    if (adapterOf(reg) == "waw_uv_lightmap")
                        inputFields += " float2 " + reg + "_lmap : TEXCOORD1;";
            }

            std::vector<std::string> init;
            for (const auto& [reg, decl] : inputs.items)
            {
                const auto adapter = adapterOf(reg);
                const std::string name = "input." + *fieldOf.Find(reg);
                if (adapter == "waw_neutral")
                {
                    std::vector<std::string> values;
                    for (const auto& c : Get(inputBindings, reg)->at("constant"))
                        values.push_back(FloatOf(c));
                    init.push_back(reg + " = float4(" + Join(values, ", ") + ");");
                }
                else if (signature)
                {
                    long long full = 0;
                    for (size_t i = 0; i < signature->size(); i++)
                        if ("link" + std::to_string(i) == *fieldOf.Find(reg))
                        {
                            full = (*signature)[i].at("width").get<long long>();
                            break;
                        }
                    init.push_back(reg + " = float4(" + name + Repeat(", 0", 4 - full) + ");");
                }
                else if (adapter == "waw_half_uv")
                {
                    init.push_back("uint2 half_" + reg + " = f32tof16(" + name + ");");
                    init.push_back(reg + " = float4(half_" + reg + ".y & 255, half_" + reg + ".y >> 8, half_" + reg + ".x & 255, half_" + reg + ".x >> 8);");
                }
                else if (adapter == "waw_uv")
                    init.push_back(reg + " = float4(" + name + ", 0, 0);");
                else if (adapter == "waw_uv_lightmap")
                    init.push_back(reg + " = float4(" + name + ", input." + reg + "_lmap * float2(2.0, 1.0));");
                else if (adapter == "waw_position")
                    init.push_back(reg + " = float4(" + name + ", 1);");
                else if (adapter == "waw_ubyte4_vector")
                {
                    init.push_back("float3 wrap_" + reg + " = " + name + " >= 0.5 ? " + name + " * 2.0 - 2.0 : " + name + " * 2.0;");
                    init.push_back(reg + " = float4((normalize(wrap_" + reg + ") + 1.0) * 127.0, 63.0);");
                }
                else
                    init.push_back(reg + " = float4(" + name + Repeat(", 0", 4 - decl.width) + ");");
            }

            std::vector<std::string> finish;
            for (const auto& [reg, decl] : outputs.items)
                finish.push_back("output." + reg + " = " + reg + "." + std::string("xyzw").substr(0, static_cast<size_t>(decl.width)) + ";");
            if (const Json* rgbScale = Section("output_rgb_scale"))
            {
                if (stage != "ps")
                    throw ShaderError("output colour scale applies to pixel programs only");
                for (size_t i = 0; i < outputs.items.size(); i++)
                {
                    const auto& [reg, decl] = outputs.items[i];
                    if (StartsWith(Upper(decl.semantic), "SV_TARGET") && decl.width == 4)
                        finish[i] = "output." + reg + " = float4(" + reg + ".xyz * " + FloatOf(*rgbScale) + ", " + reg + ".w);";
                }
            }
            std::vector<std::string> tail;
            if (Truthy(epilogue))
                if (const Json* lines = Get(epilogue, "lines"))
                    for (const auto& line : *lines)
                        tail.push_back(Str(line));

            std::vector<std::string> text(resources);
            text.push_back("struct Input { " + inputFields + " };");
            text.push_back("struct Output { " + Fields(outputs, centroid) + " };");
            text.emplace_back("Output main(Input input) {");
            for (const auto* part : {&decls, &init, &body, &tail})
                text.insert(text.end(), part->begin(), part->end());
            text.emplace_back("Output output;");
            text.insert(text.end(), finish.begin(), finish.end());
            text.emplace_back("return output;");
            text.emplace_back("}");

            Translation result;
            result.hlsl = Join(text, "\n") + "\n";
            Json report = Json::object();
            report["source_profile"] = stage + "_3_0";
            report["target_profile"] = stage + "_5_0";
            report["instruction_count"] = operations.size();
            Json in = Json::object(), out = Json::object(), sam = Json::object();
            for (const auto& [reg, decl] : inputs.items)
                in[reg] = Json::array({decl.semantic, decl.width});
            for (const auto& [reg, decl] : outputs.items)
                out[reg] = Json::array({decl.semantic, decl.width});
            for (const auto& [reg, dim] : samplers.items)
                sam[reg] = dim;
            report["inputs"] = in;
            report["outputs"] = out;
            report["constants"] = constants;
            report["samplers"] = sam;
            report["binding_status"] = bound ? "explicit_contract" : "requires_t6_bindings";
            report["partial_precision"] = "promoted_to_float32";
            result.report = std::move(report);
            return result;
        }
    } // namespace

    Translation Translate(const std::string_view assembly, const py::Json* bindings)
    {
        Translator translator{bindings, bindings != nullptr};
        return translator.Run(assembly);
    }
} // namespace w2b::shaders
