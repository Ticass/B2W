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

    Json StageSurfaceFixes(const Path& project, const Json& materials, bool colors, bool sort)
    {
        MaterialNames additive;
        if (colors)
            for (const auto& material : materials)
                if (material.value("t4_techset", std::string{}).starts_with("mc_unlit_add"))
                    additive.insert(material.value("source", material.at("name").get<std::string>()));
        const bool inspectOrder = sort && HasModelSortMaterials(materials);
        auto alpha = Json::object();
        auto sorted = Json::object();
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
                    ReplaceStagedText(paths[i], py::JsonDumpsDefault(gltf));
            }, 4);
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (changed[i] != 0)
                    alpha[weapons::PosixPath(paths[i].lexically_relative(project))] = changed[i];
            if (inspectOrder)
                sorted = ApplySurfaceOrder(project, materials, orders);
        }
        return {{"opacity", std::move(alpha)}, {"sortKeys", std::move(sorted)}};
    }

    Json StageSurfaceFixesRequest(const Path& path)
    {
        const auto request = weapons::ReadAssetJson(path);
        return StageSurfaceFixes(files::FromUtf8(request.at("project").get_ref<const std::string&>()),
                                 request.at("materials"), request.value("colors", true), request.value("sort", true));
    }
}
