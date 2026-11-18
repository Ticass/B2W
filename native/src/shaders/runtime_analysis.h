#pragma once
// SM3/SM5 disassembly analysis shared by the runtime binder (shaderruntime.py
// registers, signature, buffers, resource_names, tainted_outputs ...).

#include "core/pytext.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace w2b::shaders::runtime
{
    // dict with Python insertion order: a repeated key keeps its first place
    template <typename V> struct Ordered
    {
        std::vector<std::pair<std::string, V>> items;

        void Set(const std::string& key, V value)
        {
            for (auto& [k, v] : items)
                if (k == key)
                {
                    v = std::move(value);
                    return;
                }
            items.emplace_back(key, std::move(value));
        }
        const V* Find(const std::string& key) const
        {
            for (const auto& [k, v] : items)
                if (k == key)
                    return &v;
            return nullptr;
        }
    };

    struct Register
    {
        std::string name;
        int row;
    };
    struct Field
    {
        int slot, offset, size;
        bool operator==(const Field&) const = default;
    };
    struct Resource
    {
        std::string name, dim;
    };

    // D3DX constant table register names and array/matrix row counts
    const Ordered<Register>& Registers(const std::string& assembly);
    // KEY -> (component count, type) of an Input/Output signature
    std::map<std::string, std::pair<int, std::string>> Signature(const std::string& assembly, const std::string& kind);
    // D3D compiler reflection: uniform field -> (buffer slot, offset, size)
    const std::map<std::string, Field>& Buffers(const std::string& assembly);
    // T6 reflection: texture slot -> (name, dimension), in listing order
    std::vector<std::pair<int, Resource>> ResourceNames(const std::string& assembly);
    // output components depending on any of the source registers
    std::set<std::pair<std::string, char>> TaintedOutputs(const std::string& assembly, const std::set<std::string>& sources);
    bool WritesComponent(const std::string& assembly, const std::string& reg, char component);

    // repr((name, row)) or "None"
    std::string RegisterRepr(const Register* reg);
} // namespace w2b::shaders::runtime
