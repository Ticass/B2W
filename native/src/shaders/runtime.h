#pragma once
// Port of shaderruntime.py: bind translated WaW programs to measured native
// T6 material passes (contracts, adapters, technique set rewrite).

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <array>
#include <vector>

namespace w2b::shaders
{
    // shaderruntime.bind_material (with nativeprobes.bind on the result):
    // rewrites output_material's technique set and returns the runtime report
    py::Json BindMaterial(const py::Json& sourceMaterial, py::Json& outputMaterial, const std::vector<std::filesystem::path>& roots,
                          const std::filesystem::path& projectRoot, const std::filesystem::path& nativeRoot,
                          const std::optional<std::array<double, 4>>& falloffPlacement = std::nullopt, bool lightShadows = true,
                          bool trailGeometry = false);
    // shaderruntime.apply_source_pass_states: the T6 slots changed
    std::vector<int> ApplySourcePassStates(const py::Json& sourceMaterial, py::Json& outputMaterial, const py::Json& runtime);
    // nativeprobes.bind: native probe readers adapted to WaW cube channels
    int BindNativeProbes(py::Json& material, const std::filesystem::path& project, const std::filesystem::path& stock);
    // a stock program disassembly, cached by path
    const std::string& AssemblyOf(const std::filesystem::path& path);
} // namespace w2b::shaders
