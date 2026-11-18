#pragma once
// impactmaterials.bind and nativeprobes.bind_native_materials (see shaderbind.cpp).

#include <filesystem>
#include <map>
#include <string>

namespace w2b::pipeline
{
    // overlay material path ("materials/<name>.json") -> source material name
    std::map<std::string, std::string> BindImpactMaterials(const std::filesystem::path& project, const std::filesystem::path& dump,
                                                           const std::filesystem::path& overlay, const std::filesystem::path& stock);
    // overlay material path -> adapted passes
    std::map<std::string, int> BindNativeProbeMaterials(const std::filesystem::path& dump, const std::filesystem::path& overlay, const std::filesystem::path& stock);
} // namespace w2b::pipeline
