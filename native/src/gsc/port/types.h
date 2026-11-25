#pragma once
// What the values of a script may be: a flow-insensitive inference over one
// script. A value is a set of types it is known to take, and whether it may be
// anything else as well (parameters, what other scripts store, unknown calls).

#include "gsc/ast.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace w2b::gsc::port
{
    enum TypeBits : uint8_t
    {
        TYPE_NUMBER = 1,
        TYPE_STRING = 2, // and localized strings
        TYPE_VECTOR = 4,
        TYPE_ARRAY = 8,
        TYPE_OBJECT = 16,
        TYPE_OTHER = 32, // functions, animations
    };

    struct Types
    {
        uint8_t known = 0;
        bool open = false; // may also be anything

        bool Known(const uint8_t type) const
        {
            return (known & type) != 0;
        }
        bool May(const uint8_t type) const
        {
            return open || Known(type);
        }
        // returns whether it grew
        bool Join(const Types& other)
        {
            const auto k = known;
            const auto o = open;
            known |= other.known;
            open = open || other.open;
            return k != known || o != open;
        }
    };

    class TypeInference
    {
    public:
        TypeInference(const Ast& ast, const std::vector<Token>& tokens);

        // the expression `id` in `function`
        Types Of(NodeId function, NodeId id) const;

    private:
        using Names = std::unordered_map<std::string, Types>;

        Types Expression(const Names& locals, NodeId id) const;
        Types Call(NodeId id) const;
        bool Collect(NodeId function);

        const Ast& ast_;
        const std::vector<Token>& tokens_;
        std::unordered_map<NodeId, Names> locals_;       // by function
        Names fields_;                                   // by field name, over the script
        std::unordered_map<std::string, Types> returns_; // by function name
        std::unordered_map<std::string, NodeId> functions_;
    };
} // namespace w2b::gsc::port
