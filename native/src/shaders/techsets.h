#pragma once
// Technique sets as the converter reads and writes them.
//
// T6TechniqueSet is the document the BO2 linker fork reads
// (techniquesets/<name>.json): stock BO2 donors load into it, the shader
// runtime rebinds passes, and it is written back with sorted keys, so the
// content hash that names a runtime technique set stays the one it was.
//
// WawTechniqueSet is the T4 dumper's waw_techniquesets/<name>.json: the WaW
// passes whose programs and arguments the runtime translates.

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace w2b::shaders
{
    // a pass argument: type 0/6 material constants, 2 material textures, 3 code
    // constants, 4 code textures, 5 literals; value is the code index or name hash
    struct T6PassArg
    {
        long long buffer = 0, location = 0, size = 0, type = 0, value = 0;
        bool operator==(const T6PassArg&) const = default;
    };

    struct T6VertexDecl
    {
        bool hasOptionalSource = false, isLoaded = false;
        std::vector<std::pair<long long, long long>> routing; // (source, dest), sixteen entries
        long long streamCount = 0;
        bool operator==(const T6VertexDecl&) const = default;
    };

    struct T6Pass
    {
        std::vector<T6PassArg> args; // per-primitive, per-object, then stable arguments
        long long customSamplerFlags = 0, materialType = 0, perObjArgCount = 0, perPrimArgCount = 0, precompiledIndex = 0, stableArgCount = 0;
        std::string pixelShader, vertexShader;
        T6VertexDecl vertexDecl;
        bool operator==(const T6Pass&) const = default;
    };

    struct T6Technique
    {
        long long flags = 0, passCount = 0;
        std::string name;
        std::vector<T6Pass> passes;
        bool operator==(const T6Technique&) const = default;
    };

    // a technique slot: empty in the donor ({}), dropped by the converter (null), or a technique
    struct T6TechniqueSlot
    {
        bool dropped = false;
        std::optional<T6Technique> technique;
        explicit operator bool() const
        {
            return technique.has_value();
        }
        bool operator==(const T6TechniqueSlot&) const = default;
    };

    struct T6TechniqueSet
    {
        std::string name;
        std::vector<T6TechniqueSlot> techniques;
        long long worldVertFormat = 0;
        bool operator==(const T6TechniqueSet&) const = default;
    };

    T6TechniqueSet T6TechniqueSetFromJson(const py::Json& data);
    py::Json ToJson(const T6TechniqueSet& set);
    T6TechniqueSet ReadT6TechniqueSet(const std::filesystem::path& path);
    // the programs the set's passes run: shader_bin/ps_<name>.cso and shader_bin/vs_<name>.cso
    std::set<std::string> ProgramFiles(const T6TechniqueSet& set);

    // a WaW pass argument: a literal, a value, or a code constant's rows
    struct WawPassArg
    {
        long long dest = 0, frequency = 0, type = 0;
        std::optional<long long> value;
        std::optional<std::vector<double>> literal;
        std::optional<long long> index, firstRow, rowCount;
    };

    struct WawPass
    {
        std::string pixelShader, vertexShader;
        std::vector<WawPassArg> args;
    };

    struct WawTechnique
    {
        std::string name;
        std::vector<WawPass> passes;
    };

    struct WawTechniqueSet
    {
        std::string name;
        std::vector<std::optional<WawTechnique>> techniques; // nullopt: the slot is empty
    };

    WawTechniqueSet ReadWawTechniqueSet(const std::filesystem::path& path);
} // namespace w2b::shaders
