#include "models/surface_fixes.h"

#include "core/files.h"
#include "core/parallel.h"
#include "weapons/stage.h"

#include <windows.h>

namespace w2b::models
{
    void ReplaceStagedText(const Path& path, std::string_view text)
    {
        // Put a new file beside a possible cache hard link, then atomically
        // replace just the staged path after the complete payload was written.
        static std::atomic<unsigned long long> next = 0;
        auto temp = path;
        temp += ".w2b-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(next++);
        try
        {
            files::WriteText(temp, text);
            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                throw files::fs::filesystem_error("could not replace staged asset", path,
                    std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
        catch (...)
        {
            std::error_code error;
            files::fs::remove(temp, error);
            throw;
        }
    }

    util::Json SurfaceFixes::OpacityJson() const
    {
        auto json = Json::object();
        for (const auto& [file, vertices] : opacity)
            json[file] = vertices;
        return json;
    }

    util::Json SurfaceFixes::SortKeysJson() const
    {
        auto json = Json::object();
        for (const auto& change : sortKeys)
            json[change.material] = {change.from, change.to};
        return json;
    }

    SurfaceFixes StageSurfaceFixes(const Path& project, const std::vector<SurfaceMaterial>& materials, bool colors, bool sort)
    {
        MaterialNames additive;
        if (colors)
            for (const auto& material : materials)
                if (material.t4Techset.starts_with("mc_unlit_add"))
                    additive.insert(material.wawName);
        const bool inspectOrder = sort && HasModelSortMaterials(materials);
        SurfaceFixes fixes;
        if (!additive.empty() || inspectOrder)
        {
            const auto paths = files::GlobSuffix(project / "model_export", ".gltf");
            std::vector<SurfaceOrder> orders(paths.size());
            std::vector<std::size_t> changed(paths.size());
            parallel::For(paths.size(), [&](std::size_t i)
            {
                auto gltf = weapons::ReadAssetJson(paths[i]);
                if (inspectOrder)
                    orders[i] = SurfaceMaterials(gltf);
                changed[i] = OpaqueVertexAlpha(gltf, additive);
                if (changed[i] != 0)
                    ReplaceStagedText(paths[i], util::JsonDumpsDefault(gltf));
            }, 4);
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (changed[i] != 0)
                    fixes.opacity.emplace_back(weapons::PosixPath(paths[i].lexically_relative(project)), changed[i]);
            if (inspectOrder)
                fixes.sortKeys = ApplySurfaceOrder(project, materials, orders);
        }
        return fixes;
    }
}
