// Port of Translator classification and resolution (gscport.py 1356-1618, 1943-1956).
#include "translator.h"

#include "api.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/pyre.h"
#include "rewrites.h"

#include <stdexcept>

namespace w2b::gsc::port
{
    const py::Json& ApiSection(const std::string& key)
    {
        static const py::Json none;
        const auto& api = Api();
        const auto it = api.find(key);
        return it == api.end() ? none : *it;
    }

    const py::Json* ApiGet(const py::Json& object, const std::string& key)
    {
        if (!object.is_object())
            return nullptr;
        const auto it = object.find(key);
        if (it == object.end() || it->is_null())
            return nullptr;
        return &*it;
    }

    std::optional<std::string> ApiString(const py::Json& object, const std::string& key)
    {
        const auto* value = ApiGet(object, key);
        if (!value || !value->is_string())
            return std::nullopt;
        return value->get<std::string>();
    }

    bool ApiListHas(const py::Json& list, const std::string& value)
    {
        if (!list.is_array())
            return list.is_object() && list.contains(value);
        for (const auto& item : list)
            if (item.is_string() && item.get_ref<const std::string&>() == value)
                return true;
        return false;
    }

    std::set<std::string> CoreIncludes()
    {
        return {std::begin(CORE_INCLUDES), std::end(CORE_INCLUDES)};
    }

    namespace
    {
        const py::Json& Bo2Params()
        {
            static const py::Json params = []
            {
                std::string text;
                if (!files::ReadBytes(CompatDir() / "bo2_core_params.json", text))
                    return py::Json::object();
                return py::Json::parse(text);
            }();
            return params;
        }

        std::string LoadCompat()
        {
            std::string bytes;
            if (!files::ReadBytes(CompatDir() / "_waw2bo2_compat.gsc", bytes))
                throw std::runtime_error("missing compat/_waw2bo2_compat.gsc");
            return encoding::UniversalNewlines(bytes);
        }
    } // namespace

    Translator::Translator(Sources& s, const T6Api& a, PortReport& r)
        : sources(s),
          api(a),
          report(r),
          compat(Parse(LoadCompat(), COMPAT))
    {
    }

    bool Translator::IsDone(const std::string& path) const
    {
        for (const auto& [p, _] : done)
            if (p == path)
                return true;
        return false;
    }

    std::optional<std::string> Translator::Owner(const Script& script, const std::string& name)
    {
        if (script.Find(name))
            return script.path;
        for (const auto& inc : script.includes)
        {
            const auto dep = sources.Get(inc);
            if (dep && dep->Find(name))
                return Norm(inc);
        }
        return std::nullopt;
    }

    void Translator::Want(const std::string& path)
    {
        const auto p = Norm(path);
        if (!IsDone(p) && std::find(queue.begin(), queue.end(), p) == queue.end())
            queue.push_back(p);
    }

    std::optional<std::string> Translator::LibraryFunction(const std::string& script, const std::string& name)
    {
        const auto* table = ApiGet(Api()["library_functions"], Norm(script));
        const auto replacement = table ? ApiString(*table, name) : std::nullopt;
        if (!replacement)
            return std::nullopt;
        report.rewrites[Norm(script) + "::" + name + " -> compat waw_" + *replacement] += 1;
        return std::string(COMPAT) + "::waw_" + *replacement;
    }

    std::optional<std::string> Translator::CompatName(const std::string& name) const
    {
        if (compat.Find("waw_" + name))
            return std::string(COMPAT) + "::waw_" + name;
        return std::nullopt;
    }

    std::optional<std::string> Translator::Bo2Unique(const std::string& name) const
    {
        static const pyre::Regex shared(R"(^(common_scripts\\utility|maps\\mp\\_utility|maps\\mp\\zombies\\_zm[a-z_]*|)"
                                        R"(maps\\mp\\gametypes_zm\\_[a-z_]+|maps\\mp\\_[a-z_]+)$)");
        std::vector<std::string> hits;
        for (const auto& path : api.ByName(name))
            if (shared.MatchAt(path))
                hits.push_back(path);
        if (hits.size() == 1)
            return hits[0] + "::" + name;
        return std::nullopt;
    }

    std::string Translator::StubName(const std::string& desc)
    {
        static const pyre::Regex nonword(R"(\W+)");
        return "stub_" + StripChars(nonword.Sub("_", Replace(desc, "::", "__")), "_");
    }

    std::string Translator::Stub(const std::string& desc, const std::string& /*name*/, const std::optional<int> argc, const std::string& where)
    {
        const auto stub = StubName(desc);
        const auto old = stubs.count(stub) ? stubs[stub] : std::make_pair(desc, 0);
        stubs[stub] = {old.first, std::max(old.second, argc.value_or(0))};
        report.Unsupported("GSC " + desc, where);
        return std::string(STUBS) + "::" + stub;
    }

    bool Translator::PlaysArgumentSound(const std::string& core, const std::string& name, const std::set<std::string>& seen)
    {
        static const std::set<std::string> soundBuiltins{"playsound", "playloopsound", "playsoundatposition", "playlocalsound",
                                                          "playsoundasmaster", "playsoundtoplayer", "playsoundwithnotify"};
        const auto script = sources.Get(core);
        const Function* fn = script ? script->Find(Lower(name)) : nullptr;
        if (!fn || fn->params.empty() || seen.count(Lower(name)))
            return false;
        std::set<std::string> params;
        for (const auto& p : fn->params)
            params.insert(Lower(p));
        const auto& toks = script->tokens;
        const auto arguments = [&](const long long open)
        {
            std::vector<std::vector<std::string>> args;
            std::vector<std::string> current;
            int depth = 0;
            for (long long j = open + 1; j < static_cast<long long>(toks.size()); j++)
            {
                const auto& t = toks[static_cast<size_t>(j)].text;
                if (t == "(" || t == "[")
                    depth++;
                else if (t == ")" || t == "]")
                {
                    if (depth == 0)
                    {
                        args.push_back(current);
                        return args;
                    }
                    depth--;
                }
                else if (t == "," && depth == 0)
                {
                    args.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(toks[static_cast<size_t>(j)].Low());
            }
            return args;
        };
        for (const auto& ref : References(toks, fn->bodyOpen, fn->bodyClose))
        {
            const auto i = static_cast<long long>(ref.index);
            if (i + 1 >= static_cast<long long>(toks.size()) || toks[static_cast<size_t>(i + 1)].text != "(")
                continue;
            const auto args = arguments(i + 1);
            const auto callee = Lower(ref.name);
            if (soundBuiltins.count(callee))
            {
                if (!args.empty())
                    for (const auto& t : args[0])
                        if (params.count(t))
                            return true;
            }
            else
            {
                bool passes = false;
                for (const auto& arg : args)
                    passes = passes || (arg.size() == 1 && params.count(arg[0]));
                if (passes)
                {
                    const auto owner = FunctionScript(core, ref);
                    auto next = seen;
                    next.insert(Lower(name));
                    if (owner && PlaysArgumentSound(*owner, callee, next))
                        return true;
                }
            }
        }
        return false;
    }

    std::optional<std::string> Translator::FunctionScript(const std::string& core, const Ref& ref)
    {
        const auto callee = Lower(ref.name);
        if (ref.qualifier && !ref.qualifier->empty())
        {
            const auto target = *ref.qualifier;
            const auto script = sources.Get(target);
            if (script && script->Find(callee))
                return target;
            return std::nullopt;
        }
        const auto script = sources.Get(core);
        if (!script)
            return std::nullopt;
        if (script->Find(callee))
            return core;
        for (const auto& include : script->includes)
        {
            const auto other = sources.Get(include);
            if (other && other->Find(callee))
                return include;
        }
        return std::nullopt;
    }

    std::optional<std::string> Translator::ResolveCore(const std::string& core, const std::string& name, const std::optional<int> argc,
                                                       const std::string& where, const std::set<std::string>& included, const bool sibling)
    {
        const auto& A = Api();
        if (const auto renamed = ApiString(A["function_renames"], name); renamed && !renamed->empty())
            return renamed;
        if (const auto* source = ApiGet(ApiSection("source_script_functions"), core); source && ApiListHas(*source, name))
            if (auto extracted = Extract(core, name))
                return extracted;
        if (ApiListHas(A["template_scripts"], core))
            if (auto extracted = Extract(core, name))
                return extracted;
        if (ApiListHas(A["compat_wrappers"], name))
            if (auto c = CompatName(name))
            {
                report.rewrites[core + "::" + name + " -> compat wrapper"] += 1;
                return c;
            }
        if (PlaysArgumentSound(core, name))
            if (auto extracted = Extract(core, name))
            {
                report.rewrites[core + "::" + name + " -> source (plays a caller's sound alias)"] += 1;
                return extracted;
            }
        if (SourceDrives(core, name))
            if (auto extracted = Extract(core, name))
            {
                report.rewrites[core + "::" + name + " -> source (WaW drives)"] += 1;
                return extracted;
            }
        if (SignatureDiffers(core, name))
            if (auto extracted = Extract(core, name))
            {
                report.rewrites[core + "::" + name + " -> source (signature differs from BO2's)"] += 1;
                return extracted;
            }
        for (const auto& inc : included)
            if (api.Defines(inc, name))
            {
                report.rewrites[core + "::" + name + " -> included BO2 script"] += 1;
                return std::nullopt;
            }
        const auto mapped = ApiString(A["core_scripts"], core);
        if (mapped && !mapped->empty() && api.Defines(*mapped, name))
        {
            report.rewrites[core + "::" + name + " -> " + *mapped] += 1;
            if (included.count(*mapped))
                return std::nullopt;
            return *mapped + "::" + name;
        }
        if (auto c = CompatName(name))
        {
            report.rewrites[core + "::" + name + " -> compat"] += 1;
            return c;
        }
        if (sibling)
            if (auto extracted = Extract(core, name))
                return extracted;
        if (auto unique = Bo2Unique(name))
        {
            report.rewrites[core + "::" + name + " -> " + unique->substr(0, unique->find("::"))] += 1;
            return unique;
        }
        if (auto extracted = Extract(core, name))
            return extracted;
        return Stub(core + "::" + name, name, argc, where);
    }

    std::optional<std::string> Translator::ResolveBuiltin(const std::string& name, const std::optional<int> argc, const std::string& where,
                                                          const std::set<std::string>& included, const std::optional<bool> method)
    {
        const auto& A = Api();
        if (animNeutralized && A["anim_builtins"].contains(name))
        {
            report.rewrites["builtin " + name + " -> compat (animtree not converted)"] += 1;
            return std::string(COMPAT) + "::waw_anim_" + A["anim_builtins"][name].get<std::string>();
        }
        const auto c = CompatName(name);
        if (c && (ApiListHas(A["compat_wrappers"], name) || !api.BuiltinAccepts(name, argc.value_or(0), method).value_or(false)))
        {
            report.rewrites["builtin " + name + " -> compat"] += 1;
            return c;
        }
        const auto accepts = api.BuiltinAccepts(name, argc.value_or(0), method);
        if (accepts && *accepts)
            return std::nullopt;
        for (const auto& inc : included)
            if (api.Defines(inc, name))
                return std::nullopt;
        if (auto unique = Bo2Unique(name))
        {
            report.rewrites["builtin " + name + " -> " + unique->substr(0, unique->find("::"))] += 1;
            return unique;
        }
        if (accepts && !*accepts)
        {
            const auto range = api.BuiltinRange(name, method);
            return Stub(name + "/" + (argc ? std::to_string(*argc) : "None") + "args(T6 takes " + std::to_string(range->lo) + "-" + std::to_string(range->hi)
                            + ")",
                        name, argc, where);
        }
        return Stub(name, name, argc, where);
    }

    bool Translator::ShadowsBuiltin(const std::string& name) const
    {
        return api.BuiltinRange(Lower(name)).has_value();
    }

    bool Translator::SignatureDiffers(const std::string& core, const std::string& name)
    {
        const auto& A = Api();
        const auto mapped = ApiString(A["core_scripts"], core);
        if (!mapped || mapped->empty() || !ApiGet(ApiSection("signature_source_scripts"), core))
            return false;
        const auto* table = ApiGet(Bo2Params(), Lower(*mapped));
        const auto* bo2 = table ? ApiGet(*table, name) : nullptr;
        const auto script = sources.Get(core);
        const Function* own = script ? script->Find(name) : nullptr;
        if (!bo2 || !own)
            return false;
        for (size_t i = 0; i < own->params.size(); i++)
        {
            const auto p = Lower(own->params[i]);
            for (size_t k = 0; k < bo2->size(); k++)
                if ((*bo2)[k] == p)
                {
                    if (k != i)
                        return true;
                    break;
                }
        }
        return false;
    }
} // namespace w2b::gsc::port
