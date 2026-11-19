#pragma once
// Port of shaderruntime.py: bind translated WaW programs to measured native
// T6 material passes (contracts, adapters, technique set rewrite).

#include "core/pytext.h"
#include "materials/t6_material.h"
#include "materials/waw_material.h"
#include "shaders/output.h"

#include <filesystem>
#include <optional>
#include <array>
#include <vector>

namespace w2b::shaders
{
    // shaderruntime.bind_material (with nativeprobes.bind on the result):
    // rewrites output_material's technique set, writes the bound programs to
    // `output` and returns the runtime report
    py::Json BindMaterial(const materials::WawMaterial& sourceMaterial, materials::T6Material& outputMaterial, const std::vector<std::filesystem::path>& roots, Output& output,
                          const std::filesystem::path& nativeRoot, const std::optional<std::array<double, 4>>& falloffPlacement = std::nullopt,
                          bool lightShadows = true, bool trailGeometry = false);
    // shaderruntime.apply_source_pass_states: the T6 slots changed
    std::vector<int> ApplySourcePassStates(const materials::WawMaterial& sourceMaterial, materials::T6Material& outputMaterial, const py::Json& runtime);
    // nativeprobes.bind: native probe readers adapted to WaW cube channels;
    // reads and writes technique sets and programs in `output`
    int BindNativeProbes(materials::T6Material& material, Output& output, const std::filesystem::path& stock);
    // a stock program disassembly, cached by path
    const std::string& AssemblyOf(const std::filesystem::path& path);
} // namespace w2b::shaders
