#pragma once
// Program contracts of shaderruntime.py: how a translated WaW program reads
// its inputs, constants and textures from a native T6 pass.

#include "core/pytext.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace w2b::shaders::runtime
{
    using Json = py::Json;
    namespace fs = std::filesystem;

    const std::string& NativeProgram(const fs::path& root, const std::string& stage, const Json& pass);
    std::string EmbedLiterals(std::string assembly, const Json& sourcePass, const std::string& stage);
    Json PixelContract(const std::string& sourceAssembly, const Json& sourcePass, const Json& nativePass, const fs::path& nativeRoot, const Json* sourceMaterial);
    Json PairedPixelContract(const std::string& sourceAssembly, const Json& sourcePass, Json& nativePass, const fs::path& nativeRoot, const Json& linked,
                             const std::optional<std::string>& visibility, const Json* sourceMaterial,
                             const std::optional<std::array<double, 4>>& falloffPlacement, Json* outputMaterial);
    Json VertexContract(const std::string& sourceAssembly, const Json& sourcePass, Json& nativePass, const fs::path& nativeRoot, bool paired,
                        const Json* sourceMaterial, bool layered, bool trailGeometry);
    // (assembly, adapted pass)
    std::pair<std::string, Json> BindLocalLightRamp(std::string assembly, const Json& sourcePass, const Json* sourceMaterial);
    // (assembly, normalized)
    std::pair<std::string, bool> NormalizeSoftParticleDepth(const std::string& assembly, const std::string& pixelAssembly);
    std::string NativeFogAdapter(const std::string& hlsl, const std::string& sourceAssembly, const Json& contract, const Json& nativePass, const fs::path& nativeRoot);
    void CheckLinkage(const std::string& vertexCode, const std::string& pixelCode);
    void PruneMissingMaterialConstants(Json& nativePass, const Json& material, const std::vector<const Json*>& contracts, bool unread);
    bool IsScorchProgram(const Json* sourceMaterial);
    uint32_t WawHash(const std::string& name);
    uint32_t T6Hash(const std::string& name);
} // namespace w2b::shaders::runtime
