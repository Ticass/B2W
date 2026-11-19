#pragma once
// A converted material as the bridge report lists it (bridge_stage.report.json
// "materials"): which material file was staged, from which WaW material and
// donor, and what the shader runtime bound.

#include "core/pytext.h"

#include <optional>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct MaterialRecord
    {
        std::string name, file;                       // the staged material and its project-relative JSON
        std::optional<std::string> source;            // the WaW material, when the staged name differs
        std::optional<std::string> sourceName;        // the stock BO2 name it was isolated from
        std::optional<std::string> substitutedFrom;   // a dangling reference's stand-in
        // the WaW technique set; null for a BO2 equivalent, absent for a material that has none
        std::optional<std::optional<std::string>> t4Techset;
        std::optional<long long> t4SortKey;
        std::string t6Techset;
        py::Json shaderRuntime;                       // the shader runtime report, null when none ran
        std::optional<std::string> donor, nativeEquivalent;
        std::optional<long long> cost;
        std::optional<std::vector<std::string>> notes; // absent for records that never carry any

        // the name the WaW side knows it by
        const std::string& WawName() const
        {
            return source ? *source : name;
        }
        py::Json ToJson() const;
        static MaterialRecord FromJson(const py::Json& data);
    };

    py::Json ToJson(const std::vector<MaterialRecord>& records);
} // namespace w2b::pipeline
