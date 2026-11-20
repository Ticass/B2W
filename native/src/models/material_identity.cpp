#include "models/material_identity.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "core/parallel.h"
#include "gsc/port/rewrites.h"
#include "gsc/port/util.h"
#include "models/identities.h"
#include "models/surface_fixes.h"
#include "weapons/stage.h"

namespace w2b::models
{
    namespace
    {
        bool OwnedName(std::string_view name)
        {
            return name.starts_with("waw/") || name.starts_with("waw_world/") ||
                   name.starts_with("waw_material/") || name.starts_with("waw_fx/");
        }

        std::vector<MaterialRename> CopyMaterials(const Path& project, const Path& stock, const std::vector<MaterialFile>& entries, RenameList& renamed)
        {
            std::vector<MaterialRename> updates;
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                const auto& name = entries[i].name;
                if (OwnedName(name) || !files::fs::is_regular_file(stock / assets::MaterialPath(name)))
                    continue;
                const auto source = project / files::FromUtf8(entries[i].file);
                if (!files::fs::exists(source))
                    continue;
                const auto material = weapons::ReadAssetJson(source);
                if (!material.value("techniqueSet", std::string{}).starts_with("waw/"))
                    continue;
                auto output = "waw_world/" + name;
                std::ranges::replace(output, '*', '_');
                const auto relative = assets::MaterialPath(output);
                weapons::CopySource(source, project / relative);
                updates.push_back({i, output, weapons::PosixPath(relative), name});
                if (const auto known = std::ranges::find(renamed, name, &RenameList::value_type::first); known != renamed.end())
                    known->second = std::move(output);
                else
                    renamed.emplace_back(name, std::move(output));
            }
            return updates;
        }

        bool RewriteName(Json& name, const ModelNames& renamed)
        {
            if (!name.is_string())
                return false;
            const auto match = renamed.find(name.get_ref<const std::string&>());
            if (match == renamed.end())
                return false;
            name = match->second;
            return true;
        }

        void UpdateMapManifests(const Path& bsp, const ModelNames& names)
        {
            const auto maps = bsp / "lightmaps.json";
            const auto constraints = bsp / "constraints.json";
            if (files::fs::is_regular_file(maps))
            {
                auto data = weapons::ReadAssetJson(maps);
                for (auto& name : data.at("wawMaterials"))
                    RewriteName(name, names);
                ReplaceStagedText(maps, util::JsonDumps(data, 2) + "\n");
            }
            if (files::fs::is_regular_file(constraints))
            {
                auto data = weapons::ReadAssetJson(constraints);
                for (auto& constraint : data.at("constraints"))
                    RewriteName(constraint.at("material"), names);
                ReplaceStagedText(constraints, util::JsonDumps(data, 2) + "\n");
            }
        }

        void RewriteHud(const Path& project, const std::vector<std::string>& hudNames, const ModelNames& renamed)
        {
            std::map<std::string, std::string> hud;
            for (const auto& name : hudNames)
                if (const auto it = renamed.find(name); it != renamed.end())
                    hud.insert(*it);
            if (hud.empty())
                return;
            ReplaceStagedText(project / "maps/mp/waw/_waw2bo2_hud_materials.gsc", gsc::port::HudMaterialTableSource(hud));
            RewriteStagedScripts(project, [&](std::string_view source)
            {
                bool changed = false;
                auto tokens = gsc::Tokenize(source);
                for (auto& token : tokens)
                {
                    if (token.kind != gsc::Kind::String)
                        continue;
                    const auto hit = hud.find(token.text.substr(1, token.text.size() - 2));
                    if (hit != hud.end())
                    {
                        token.text = '"' + hit->second + '"';
                        changed = true;
                    }
                }
                return changed ? gsc::Emit(tokens) : std::string(source);
            });
        }

        void UpdateModelMaterials(const Path& exportRoot, const ModelNames& names)
        {
            const auto paths = files::RGlob(exportRoot, ".gltf");
            parallel::For(paths.size(), [&](std::size_t i)
            {
                const auto& path = paths[i];
                auto data = weapons::ReadAssetJson(path);
                const auto materials = data.find("materials");
                if (materials == data.end())
                    return;
                bool changed = false;
                for (auto& material : *materials)
                    if (const auto name = material.find("name"); name != material.end())
                        changed |= RewriteName(*name, names);
                if (changed)
                    ReplaceStagedText(path, util::JsonDumpsDefault(data));
            }, 4);
        }

        std::string RewriteFbx(std::string_view source, const ModelNames& names)
        {
            constexpr std::string_view marker = "Material::";
            std::string output;
            output.reserve(source.size());
            std::size_t begin = 0, search = 0;
            while ((search = source.find(marker, search)) != source.npos)
            {
                search += marker.size();
                const auto end = source.find('"', search);
                if (end == source.npos)
                    break;
                // Replacements all target an owned prefix that is excluded from
                // the mapping, so no later substitution can change them.
                if (const auto found = names.find(source.substr(search, end - search)); found != names.end())
                {
                    output += source.substr(begin, search - begin);
                    output += found->second;
                    begin = search = end;
                }
            }
            output += source.substr(begin);
            return output;
        }
    }

    MaterialIsolation IsolateMaterialNames(const Path& project, const Path& stock, const std::vector<MaterialFile>& materials,
                                           const std::vector<std::string>& hudNames)
    {
        RenameList renamed;
        auto updates = CopyMaterials(project, stock, materials, renamed);
        if (!renamed.empty())
        {
            const ModelNames names(renamed.begin(), renamed.end());
            const auto bsp = project / "BSP";
            UpdateMapManifests(bsp, names);
            RewriteHud(project, hudNames, names);
            UpdateModelMaterials(project / "model_export", names);
            const auto map = bsp / "map_gfx.fbx";
            if (files::fs::is_regular_file(map))
                ReplaceStagedText(map, RewriteFbx(files::ReadText(map), names));
            for (const auto& path : files::RGlob(project / "fx", ".efx"))
            {
                auto text = files::ReadText(path);
                for (const auto& [old, value] : renamed)
                    text = gsc::port::Replace(std::move(text), '"' + old + '"', '"' + value + '"');
                ReplaceStagedText(path, text);
            }
        }
        return {std::move(renamed), std::move(updates)};
    }
}
