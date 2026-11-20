#pragma once
// Program contracts: how a translated WaW program reads
// its inputs, constants and textures from a native T6 pass.

#include "core/text.h"
#include "materials/t6_material.h"
#include "materials/waw_material.h"
#include "shaders/techsets.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace w2b::shaders::runtime
{
    using Json = util::Json;
    namespace fs = std::filesystem;

    const std::string& NativeProgram(const fs::path& root, const std::string& stage, const T6Pass& pass);
    std::string EmbedLiterals(std::string assembly, const WawPass& sourcePass, const std::string& stage);
    Json PixelContract(const std::string& sourceAssembly, const WawPass& sourcePass, const T6Pass& nativePass, const fs::path& nativeRoot, const materials::WawMaterial& sourceMaterial);
    Json PairedPixelContract(const std::string& sourceAssembly, const WawPass& sourcePass, T6Pass& nativePass, const fs::path& nativeRoot, const Json& linked,
                             const std::optional<std::string>& visibility, const materials::WawMaterial& sourceMaterial,
                             const std::optional<std::array<double, 4>>& falloffPlacement, materials::T6Material* outputMaterial);
    Json VertexContract(const std::string& sourceAssembly, const WawPass& sourcePass, T6Pass& nativePass, const fs::path& nativeRoot, bool paired,
                        const materials::WawMaterial& sourceMaterial, bool layered, bool trailGeometry);
    // (assembly, adapted pass)
    std::pair<std::string, WawPass> BindLocalLightRamp(std::string assembly, const WawPass& sourcePass, const materials::WawMaterial& sourceMaterial);
    // (assembly, normalized)
    std::pair<std::string, bool> NormalizeSoftParticleDepth(const std::string& assembly, const std::string& pixelAssembly);
    std::string NativeFogAdapter(const std::string& hlsl, const std::string& sourceAssembly, const Json& contract, const T6Pass& nativePass, const fs::path& nativeRoot);
    void CheckLinkage(const std::string& vertexCode, const std::string& pixelCode);
    void PruneMissingMaterialConstants(T6Pass& nativePass, const materials::T6Material& material, const std::vector<const Json*>& contracts, bool unread);
    bool IsScorchProgram(const materials::WawMaterial& sourceMaterial);
    uint32_t WawHash(const std::string& name);
    uint32_t T6Hash(const std::string& name);
} // namespace w2b::shaders::runtime
