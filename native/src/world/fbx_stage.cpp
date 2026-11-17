#include "world/fbx_stage.h"

#include "core/files.h"
#include "core/pytext.h"
#include "world/fbx_writer.h"
#include "world/render_world.h"

#include <fstream>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        using py::Json;

        FbxMesh ReadMesh(const Json& entry)
        {
            FbxMesh mesh{entry.at("material").get<std::string>(), entry.at("brush").get<int>(),
                         entry.at("light").get<int>(), entry.at("probe").get<int>(), entry.at("lightmap").get<int>(),
                         entry.at("hidden").get<bool>(), entry.at("white").get<bool>(), {}};
            const auto& surfaces = entry.at("surfaces");
            if (surfaces.empty())
                throw std::runtime_error("FBX mesh has no source surfaces");
            mesh.surfaces.reserve(surfaces.size());
            for (const auto& s : surfaces)
                mesh.surfaces.push_back({s.at(0).get<std::uint32_t>(), s.at(1).get<std::uint32_t>(),
                                         s.at(2).get<std::uint32_t>(), s.at(3).get<int>(), s.at(4).get<int>()});
            return mesh;
        }
    }

    void StageWorldFbx(const files::fs::path& request)
    {
        const auto args = Json::parse(files::ReadText(request));
        const auto world = ReadRenderWorld(files::FromUtf8(args.at("gfx").get<std::string>()),
                                           args.at("compensated").get<bool>());
        const auto& entries = args.at("meshes");
        std::vector<FbxMesh> meshes;
        meshes.reserve(entries.size());
        for (const auto& row : entries)
            meshes.push_back(ReadMesh(row));
        std::ofstream output(request.parent_path() / "world.fbx", std::ios::binary);
        WriteWorldFbx(output, {world.geometry.vertices, world.geometry.indices, world.layerData}, meshes);
        if (!output.flush())
            throw std::runtime_error("could not flush world FBX");
    }
}
