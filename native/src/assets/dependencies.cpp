#include "assets/dependencies.h"

#include "core/files.h"
#include "effects/effect.h"
#include "weapons/stage.h"

#include <algorithm>

namespace w2b::assets
{
    namespace
    {
        using py::Json;

        void AddField(AssetSet& assets, std::string_view kind, const Json& fields, std::string_view key)
        {
            const auto value = fields.find(key);
            if (value != fields.end() && value->is_string() && !value->get_ref<const std::string&>().empty())
                assets.emplace(kind, value->get_ref<const std::string&>());
        }

        void AddField(AssetSet& assets, std::string_view kind, const weapons::InfoFields& fields, std::string_view key)
        {
            if (const auto* value = fields.Find(key); value && !value->empty())
                assets.emplace(kind, *value);
        }

        AssetSet ReadFlameTable(const Path& source)
        {
            const auto fields = weapons::ReadInfo(source, "FLAMETABLEFILE");
            AssetSet dependencies;
            for (const auto key : weapons::FlameMaterialKeys)
                AddField(dependencies, "material", fields, key);
            for (const auto key : weapons::FlameSoundKeys)
                AddField(dependencies, "sound", fields, key);
            return dependencies;
        }

        AssetSet ReadMaterial(const Path& source)
        {
            const auto material = weapons::ReadAssetJson(source);
            AssetSet dependencies;
            if (const auto techset = material.find("techniqueSet"); techset != material.end() && techset->is_string())
            {
                const auto& value = techset->get_ref<const std::string&>();
                if (!value.empty())
                    dependencies.emplace("techniqueset", value.substr(std::min(value.find_first_not_of(','), value.size())));
            }
            if (const auto textures = material.find("textures"); textures != material.end())
                for (const auto& texture : *textures)
                {
                    const auto image = texture.find("image");
                    if (image == texture.end() || !image->is_string())
                        continue;
                    const auto& value = image->get_ref<const std::string&>();
                    if (!value.empty())
                        dependencies.emplace("image", value.starts_with(',') ? value.substr(1) : value);
                }
            return dependencies;
        }

        AssetSet ReadSound(const Path& source)
        {
            const auto sound = weapons::ReadAssetJson(source);
            AssetSet dependencies;
            if (const auto aliases = sound.find("aliases"); aliases != sound.end())
                for (const auto& alias : *aliases)
                {
                    AddField(dependencies, "sound", alias, "secondaryAliasName");
                    AddField(dependencies, "sound", alias, "chainAliasName");
                    const auto file = alias.find("soundFile");
                    if (file == alias.end() || !file->is_object())
                        continue;
                    const auto name = file->find("name");
                    if (file->value("type", Json()) == 1 && name != file->end() && name->is_string() &&
                        !name->get_ref<const std::string&>().empty())
                        dependencies.emplace("loadedsound", name->get_ref<const std::string&>());
                    else
                        AddField(dependencies, "loadedsound", *file, "primedName");
                }
            return dependencies;
        }

        AssetSet ReadEffect(const Path& source)
        {
            const auto references = effects::FindDependencies(weapons::ReadAssetJson(source));
            AssetSet dependencies;
            const auto add = [&](std::string_view kind, const auto& values) {
                for (const auto& value : values)
                    dependencies.emplace(kind, value);
            };
            add("material", references.materials);
            add("xmodel", references.models);
            add("sound", references.sounds);
            add("fx", references.effects);
            return dependencies;
        }

        void ReadNotetracks(const Path& source, std::string_view value, AssetSet& dependencies)
        {
            for (const auto line : py::SplitLines(value))
            {
                const auto tokens = weapons::NotetrackWords(line);
                if (tokens.size() != 2)
                    throw weapons::WeaponError("invalid notetrack sound dependency in " + files::Utf8(source) + ": " + py::StringRepr(line));
                dependencies.emplace("sound", tokens[1]);
            }
        }
    }

    AssetSet DependencyReader::Read(std::string_view kind, const Path& source)
    {
        using Reader = AssetSet (*)(const Path&);
        static const std::map<std::string_view, Reader, std::less<>> readers{
            {"flametable", ReadFlameTable}, {"material", ReadMaterial}, {"sound", ReadSound}, {"fx", ReadEffect}};
        const auto reader = readers.find(kind);
        return reader == readers.end() ? AssetSet{} : reader->second(source);
    }

    AssetSet DependencyReader::ReadWeapon(const Path& source, const std::function<void(std::string_view, AssetSet&)>& bounce)
    {
        if (schema_.empty())
            schema_ = weapons::ReadSchema(base_ / "vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon/WeaponFields.h");
        const auto fields = weapons::ReadInfo(source);
        AssetSet dependencies;
        for (const auto& [key, value] : fields)
        {
            if (value.empty())
                continue;
            const auto& type = weapons::Field(schema_, key);
            auto child = weapons::DependencyKind(type);
            if (std::ranges::find(weapons::FlameTableKeys, key) != weapons::FlameTableKeys.end())
                child = "flametable";
            if (!child.empty())
                dependencies.emplace(child, value);
            if (type == "WFT_BOUNCE_SOUND")
                bounce(value, dependencies);
            if (key == "altWeapon")
                dependencies.emplace("weapon", value);
            if (key == "aiVsAiAccuracyGraph" || key == "aiVsPlayerAccuracyGraph")
                dependencies.emplace(key == "aiVsAiAccuracyGraph" ? "accuracy_aivsai" : "accuracy_aivsplayer", value);
            if (type == "WFT_NOTETRACKSOUNDMAP")
                ReadNotetracks(source, value, dependencies);
        }
        return dependencies;
    }

    AssetSet DependencyReader::ReadModel(const Json& model)
    {
        AssetSet dependencies;
        AddField(dependencies, "physpreset", model, "physPreset");
        return dependencies;
    }

    void DependencyReader::AddGeometry(const Path& source, AssetSet& assets)
    {
        const auto geometry = weapons::ReadAssetJson(source);
        if (const auto materials = geometry.find("materials"); materials != geometry.end())
            for (const auto& material : *materials)
                assets.emplace("material", material.at("name").get_ref<const std::string&>());
    }
}
