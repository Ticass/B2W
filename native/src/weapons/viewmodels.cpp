#include "weapons/stage.h"

#include "core/asset_paths.h"
#include "core/files.h"

#include <algorithm>
#include <regex>

namespace w2b::weapons
{
    namespace
    {
        constexpr std::string_view VariantPrefix = "viewmodel_shadowless/";
        constexpr std::string_view MaterialPrefix = "waw_material/";

        using assets::MaterialPath;

        void WriteJson(const path& target, const Json& data)
        {
            files::WriteText(target, py::JsonDumps(data, 2) + "\n");
        }

        std::string OriginalMaterial(std::string name)
        {
            if (name.starts_with(MaterialPrefix))
                name.erase(0, MaterialPrefix.size());
            return name;
        }

        bool CastsShadow(const Json& material)
        {
            const auto flags = material.find("gameFlags");
            return flags != material.end() && std::ranges::find(*flags, Json("CASTS_SHADOW")) != flags->end();
        }

        using Index = std::map<std::string, std::size_t, std::less<>>;

        Index IndexAssets(const Json& rows)
        {
            Index index;
            for (std::size_t i = 0; i < rows.size(); ++i)
                index[rows.at(i).at("name").get_ref<const std::string&>()] = i;
            return index;
        }

        class ShadowVariants
        {
        public:
            explicit ShadowVariants(path content)
                : content_(std::move(content)),
                  stage_(ReadAssetJson(content_ / "weapons.stage.json")),
                  visual_(ReadAssetJson(content_ / "weapons.visuals.json")),
                  models_(IndexAssets(stage_.at("models").at("models"))),
                  materials_(IndexAssets(visual_.at("materials")))
            {
            }

            Json Run()
            {
                for (const auto& weapon : stage_.at("weapons"))
                    RewriteWeapon(content_ / "weapons" / files::FromUtf8(weapon.at("name").get_ref<const std::string&>()));
                stage_["viewmodel_shadow_variants"] = replacements_;
                visual_["viewmodel_shadow_materials"] = clones_;
                WriteJson(content_ / "weapons.stage.json", stage_);
                WriteJson(content_ / "weapons.visuals.json", visual_);
                return {{"models", std::move(replacements_)}, {"materials", std::move(clones_)}};
            }

        private:
            void RewriteWeapon(const path& source)
            {
                if (!std::filesystem::is_regular_file(source))
                    return;
                auto fields = ReadInfo(source);
                bool changed = false;
                static const std::regex gunSlot(R"(^gunModel([2-9]|1[0-6])?$)");
                static const auto prefix = OutputName("xmodel", std::string(VariantPrefix));
                for (auto& [key, value] : fields.items())
                {
                    const auto& name = value.get_ref<const std::string&>();
                    if (!key.starts_with("gunModel") || !std::regex_match(key, gunSlot) || name.starts_with(prefix))
                        continue;
                    const auto model = models_.find(name);
                    if (model == models_.end())
                        continue;
                    if (!replacements_.contains(name))
                        MakeModel(name, model->second);
                    if (const auto replacement = replacements_.find(name); replacement != replacements_.end())
                    {
                        value = *replacement;
                        changed = true;
                    }
                }
                if (changed)
                    files::WriteText(source, WriteInfo(fields));
            }

            const Json& Material(const std::string& name)
            {
                if (const auto cached = definitions_.find(name); cached != definitions_.end())
                    return cached->second;
                return definitions_.emplace(name, ReadAssetJson(content_ / MaterialPath(name))).first->second;
            }

            void MakeMaterial(const std::string& oldName, const std::string& newName)
            {
                if (clones_.contains(newName))
                    return;
                auto definition = Material(oldName);
                auto& flags = definition.at("gameFlags");
                flags.erase(std::ranges::find(flags, Json("CASTS_SHADOW")));
                const auto relative = MaterialPath(newName);
                WriteJson(content_ / relative, definition);
                auto& rows = visual_.at("materials");
                auto metadata = rows.at(materials_.at(oldName));
                metadata["name"] = newName;
                metadata["file"] = PosixPath(relative);
                metadata["notes"].push_back("first-person surface: no self-shadow casting");
                rows.push_back(std::move(metadata));
                auto& images = visual_.at("material_images");
                auto copiedImages = images.value(oldName, Json::array());
                images[newName] = std::move(copiedImages);
                clones_[newName] = oldName;
            }

            void MakeModel(const std::string& name, std::size_t index)
            {
                auto& models = stage_.at("models").at("models");
                auto metadata = models.at(index);
                std::vector<std::string> originalMaterials;
                for (const auto& material : metadata.at("materials"))
                {
                    auto fullName = OutputName("material", material.get_ref<const std::string&>());
                    if (!materials_.contains(fullName) || !std::filesystem::is_regular_file(content_ / MaterialPath(fullName)))
                        return;
                    originalMaterials.push_back(std::move(fullName));
                }
                if (!std::ranges::any_of(originalMaterials, [&](const auto& material) { return CastsShadow(Material(material)); }))
                    return;

                auto model = ReadAssetJson(content_ / "xmodel" / files::FromUtf8(name + ".json"));
                const auto cloneKey = std::string(VariantPrefix) + metadata.at("waw").get_ref<const std::string&>();
                const auto cloneName = OutputName("xmodel", cloneKey);
                Names clonedMaterials;
                auto& lods = model.at("lods");
                for (std::size_t i = 0; i < lods.size(); ++i)
                {
                    auto& lod = lods.at(i);
                    auto geometry = ReadAssetJson(content_ / files::FromUtf8(lod.at("file").get_ref<const std::string&>()));
                    if (const auto materials = geometry.find("materials"); materials != geometry.end())
                        for (auto& material : *materials)
                        {
                            auto& entry = material.at("name");
                            const auto& oldName = entry.get_ref<const std::string&>();
                            if (CastsShadow(Material(oldName)))
                            {
                                auto newName = OutputName("material", std::string(VariantPrefix) + OriginalMaterial(oldName));
                                MakeMaterial(oldName, newName);
                                entry = std::move(newName);
                            }
                            clonedMaterials.insert(OriginalMaterial(entry.get_ref<const std::string&>()));
                        }
                    const auto relative = "model_export/" + cloneName + "_lod" + std::to_string(i) + ".gltf";
                    files::WriteBytes(content_ / files::FromUtf8(relative), py::JsonDumps(geometry, -1));
                    lod["file"] = relative;
                }
                WriteJson(content_ / "xmodel" / files::FromUtf8(cloneName + ".json"), model);
                metadata["name"] = cloneName;
                metadata["waw"] = cloneKey;
                metadata["materials"] = std::move(clonedMaterials);
                models.push_back(std::move(metadata));
                replacements_[name] = cloneName;
            }

            path content_;
            Json stage_;
            Json visual_;
            Index models_;
            Index materials_;
            std::map<std::string, Json, std::less<>> definitions_;
            Json replacements_ = Json::object();
            Json clones_ = Json::object();
        };
    }

    Json StageShadowVariants(const path& content)
    {
        return ShadowVariants(content).Run();
    }
}
