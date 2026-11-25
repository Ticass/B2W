#include "types.h"

namespace w2b::gsc::port
{
    namespace
    {
        constexpr Types ANYTHING{0, true};

        // what T4's builtins return; undefined, after a fault, aside
        const std::unordered_map<std::string, uint8_t>& Builtins()
        {
            static const std::unordered_map<std::string, uint8_t> table{
                {"isdefined", TYPE_NUMBER},     {"isalive", TYPE_NUMBER},         {"isplayer", TYPE_NUMBER},        {"issubstr", TYPE_NUMBER},
                {"isarray", TYPE_NUMBER},       {"isstring", TYPE_NUMBER},        {"isai", TYPE_NUMBER},            {"distance", TYPE_NUMBER},
                {"distance2d", TYPE_NUMBER},    {"distancesquared", TYPE_NUMBER}, {"gettime", TYPE_NUMBER},         {"getdvarint", TYPE_NUMBER},
                {"getdvarfloat", TYPE_NUMBER},  {"int", TYPE_NUMBER},             {"float", TYPE_NUMBER},           {"randomint", TYPE_NUMBER},
                {"randomintrange", TYPE_NUMBER}, {"randomfloat", TYPE_NUMBER},    {"randomfloatrange", TYPE_NUMBER}, {"vectordot", TYPE_NUMBER},
                {"length", TYPE_NUMBER},        {"lengthsquared", TYPE_NUMBER},   {"cos", TYPE_NUMBER},             {"sin", TYPE_NUMBER},
                {"tan", TYPE_NUMBER},           {"acos", TYPE_NUMBER},            {"asin", TYPE_NUMBER},            {"atan", TYPE_NUMBER},
                {"sqrt", TYPE_NUMBER},          {"abs", TYPE_NUMBER},             {"getentitynumber", TYPE_NUMBER}, {"getweaponammoclip", TYPE_NUMBER},
                {"getweaponammostock", TYPE_NUMBER}, {"istouching", TYPE_NUMBER}, {"getdvar", TYPE_STRING},         {"tolower", TYPE_STRING},
                {"getsubstr", TYPE_STRING},     {"getcurrentweapon", TYPE_STRING}, {"weaponclass", TYPE_STRING},    {"tablelookup", TYPE_STRING},
                {"anglestoforward", TYPE_VECTOR}, {"anglestoright", TYPE_VECTOR}, {"anglestoup", TYPE_VECTOR},      {"vectortoangles", TYPE_VECTOR},
                {"vectornormalize", TYPE_VECTOR}, {"vectorscale", TYPE_VECTOR},   {"geteye", TYPE_VECTOR},          {"gettagorigin", TYPE_VECTOR},
                {"getplayerangles", TYPE_VECTOR}, {"getorigin", TYPE_VECTOR},     {"getvelocity", TYPE_VECTOR},     {"getentarray", TYPE_ARRAY},
                {"getarraykeys", TYPE_ARRAY},   {"getnodearray", TYPE_ARRAY},     {"getaiarray", TYPE_ARRAY},       {"getaispeciesarray", TYPE_ARRAY},
                {"strtok", TYPE_ARRAY},         {"getplayers", TYPE_ARRAY},       {"get_players", TYPE_ARRAY},      {"getspawnerarray", TYPE_ARRAY},
                {"bullettrace", TYPE_ARRAY},    {"getent", TYPE_OBJECT},          {"getnode", TYPE_OBJECT},         {"spawn", TYPE_OBJECT},
                {"spawnstruct", TYPE_OBJECT},   {"newhudelem", TYPE_OBJECT},      {"newclienthudelem", TYPE_OBJECT}, {"spawnfx", TYPE_OBJECT}};
            return table;
        }

        Types Known(const uint8_t types)
        {
            return Types{types, false};
        }
    } // namespace

    TypeInference::TypeInference(const Ast& ast, const std::vector<Token>& tokens) : ast_(ast), tokens_(tokens)
    {
        const auto functions = ast.Functions();
        for (const auto id : functions)
        {
            const auto name = tokens[ast[id].begin].Low();
            functions_.emplace(name, id);
            returns_.emplace(name, Types{});
            auto& locals = locals_[id];
            for (const auto kid : ast[id].kids)
                if (ast[kid].kind == NodeKind::Param)
                    locals[tokens[ast[kid].token].Low()] = ANYTHING;
        }
        // the sets only grow, so this ends
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const auto id : functions)
                grew = Collect(id) || grew;
        }
    }

    Types TypeInference::Of(const NodeId function, const NodeId id) const
    {
        static const Names none;
        const auto it = locals_.find(function);
        return Expression(it == locals_.end() ? none : it->second, id);
    }

    bool TypeInference::Collect(const NodeId function)
    {
        auto& locals = locals_[function];
        auto& returns = returns_[tokens_[ast_[function].begin].Low()];
        bool grew = false;
        // what a target holds after a store: a local of this function or a field of the script
        const auto store = [&](const NodeId target, const Types& types)
        {
            const auto& n = ast_[target];
            if (n.kind == NodeKind::Local)
                grew = locals[tokens_[n.token].Low()].Join(types) || grew;
            else if (n.kind == NodeKind::Field)
                grew = fields_[tokens_[n.token].Low()].Join(types) || grew;
        };
        std::vector<NodeId> stack{function};
        while (!stack.empty())
        {
            const auto id = stack.back();
            stack.pop_back();
            const auto& n = ast_[id];
            stack.insert(stack.end(), n.kids.begin(), n.kids.end());
            switch (n.kind)
            {
            case NodeKind::Assign:
            {
                const auto value = Expression(locals, n.kids[1]);
                const auto target = n.kids[0];
                if (ast_[target].kind == NodeKind::Index)
                {
                    // "x[k] = v" makes x an array
                    store(ast_[target].kids[0], Known(TYPE_ARRAY));
                    break;
                }
                const auto& op = tokens_[n.token].text;
                if (op == "=" || op == "+=")
                    store(target, value);
                if (op != "=")
                    store(target, Known(TYPE_NUMBER));
                break;
            }
            case NodeKind::Increment:
            case NodeKind::Decrement:
                store(n.kids[0], Known(TYPE_NUMBER));
                break;
            case NodeKind::WaitTill:
                for (size_t k = 2; k < n.kids.size(); k++)
                    store(n.kids[k], ANYTHING);
                break;
            case NodeKind::ForEach:
                for (size_t k = 0; k + 2 < n.kids.size(); k++)
                    store(n.kids[k], ANYTHING);
                break;
            case NodeKind::Return:
                if (!n.kids.empty())
                    grew = returns.Join(Expression(locals, n.kids[0])) || grew;
                break;
            default:
                break;
            }
        }
        return grew;
    }

    Types TypeInference::Call(const NodeId id) const
    {
        const auto& n = ast_[id];
        const auto& callee = ast_[n.kids[0]];
        if (n.threaded || !callee.kids.empty())
            return ANYTHING;
        const auto name = tokens_[callee.token].Low();
        if (!callee.qualifier)
            if (const auto own = returns_.find(name); own != returns_.end())
                return own->second;
        const auto builtin = Builtins().find(name);
        return builtin == Builtins().end() ? ANYTHING : Known(builtin->second);
    }

    Types TypeInference::Expression(const Names& locals, const NodeId id) const
    {
        const auto& n = ast_[id];
        switch (n.kind)
        {
        case NodeKind::Undefined:
            return {};
        case NodeKind::Int:
        case NodeKind::Float:
        case NodeKind::True:
        case NodeKind::False:
        case NodeKind::Not:
        case NodeKind::Complement:
        case NodeKind::Size:
            return Known(TYPE_NUMBER);
        case NodeKind::String:
        case NodeKind::LocalizedString:
            return Known(TYPE_STRING);
        case NodeKind::Self:
        case NodeKind::Level:
        case NodeKind::Anim:
            return Known(TYPE_OBJECT);
        case NodeKind::Game:
        case NodeKind::EmptyArray:
            return Known(TYPE_ARRAY);
        case NodeKind::AnimRef:
        case NodeKind::AnimTree:
        case NodeKind::FunctionRef:
            return Known(TYPE_OTHER);
        case NodeKind::List:
            return n.kids.size() == 1 ? Expression(locals, n.kids[0]) : Known(TYPE_VECTOR);
        case NodeKind::Local:
        {
            const auto it = locals.find(tokens_[n.token].Low());
            return it == locals.end() ? Types{} : it->second;
        }
        case NodeKind::Field:
        {
            // engine fields and other scripts' stores are not seen
            const auto it = fields_.find(tokens_[n.token].Low());
            return it == fields_.end() ? ANYTHING : Types{it->second.known, true};
        }
        case NodeKind::Call:
            return Call(id);
        case NodeKind::Conditional:
        {
            auto out = Expression(locals, n.kids[1]);
            out.Join(Expression(locals, n.kids[2]));
            return out;
        }
        case NodeKind::Binary:
        {
            const auto& op = tokens_[n.token].text;
            if (op == "+")
            {
                // a string with anything is a string
                auto out = Expression(locals, n.kids[0]);
                out.Join(Expression(locals, n.kids[1]));
                out.known &= TYPE_NUMBER | TYPE_STRING | TYPE_VECTOR;
                if (out.open)
                    out.known |= TYPE_NUMBER;
                return out;
            }
            if (op == "*" || op == "-" || op == "/")
                return Known(TYPE_NUMBER | TYPE_VECTOR);
            return Known(TYPE_NUMBER);
        }
        default: // an element of an array, a string or a vector
            return ANYTHING;
        }
    }
} // namespace w2b::gsc::port
