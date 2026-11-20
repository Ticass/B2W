// WaW materials behind the source roots (see material_sources.h).
#include "extract/material_sources.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "core/text.h"
#include "materials/waw_material_json.h"
#include "oat/waw_assets.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace w2b::extract
{
    namespace
    {
        // the name a material is found by: case-insensitive like the dump
        // files, and a generated "*a_b(…)" goes by "*a_b" (assets::MaterialPath)
        std::string Key(const std::string& name)
        {
            auto key = name.starts_with('*') ? name.substr(0, name.find('(')) : name;
            std::ranges::transform(key, key.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return key;
        }

        bool ListsMaterials(const util::Json& inputs)
        {
            const auto& kinds = inputs.value("assets", util::Json());
            if (!kinds.is_string())
                return true; // every kind was extracted
            for (const auto kind : util::Split(kinds.get_ref<const std::string&>(), ","))
                if (kind == "material")
                    return true;
            return false;
        }
    } // namespace

    MaterialSources::MaterialSources(const std::vector<fs::path>& roots)
    {
        for (const auto& folder : roots)
        {
            Root root{folder, std::nullopt, {}};
            std::string text;
            if (files::ReadBytes(folder / "extraction.json", text))
            {
                const auto receipt = util::Json::parse(text);
                const auto& inputs = receipt.at("inputs");
                const auto fastfile = files::FromUtf8(inputs.at("file").get<std::string>());
                std::error_code e;
                if (fs::is_regular_file(fastfile, e))
                {
                    root.fastfile = fastfile;
                    if (ListsMaterials(inputs))
                    {
                        const auto& index = receipt.value("index", util::Json::object());
                        const auto names = index.contains("material") || inputs.value("list", false)
                                               ? index.value("material", util::Json::array()).get<std::vector<std::string>>()
                                               : oat::MaterialNames({fastfile, {}});
                        for (const auto& name : names)
                            root.materials.emplace(Key(name), name);
                    }
                }
            }
            roots_.push_back(std::move(root));
        }
    }

    std::optional<std::pair<const MaterialSources::Root*, std::string>> MaterialSources::Locate(const std::string& name) const
    {
        const auto key = Key(name);
        const auto file = assets::MaterialPath(name);
        for (const auto& root : roots_)
        {
            std::error_code e;
            if (!root.fastfile)
            {
                if (fs::is_regular_file(root.folder / file, e))
                    return std::pair{&root, name};
            }
            else if (const auto found = root.materials.find(key); found != root.materials.end())
                return std::pair{&root, found->second};
        }
        return std::nullopt;
    }

    materials::WawMaterial MaterialSources::ReadFrom(const Root& root, const std::string& name)
    {
        if (!root.fastfile)
            return materials::FromOatJson(util::Json::parse(files::ReadText(root.folder / assets::MaterialPath(name))));
        auto material = oat::ReadMaterial({*root.fastfile, {}}, name);
        if (!material)
            throw std::runtime_error("material " + name + ": listed by " + files::Utf8(root.folder / "extraction.json") + " but absent from " +
                                     files::Utf8(*root.fastfile));
        return std::move(*material);
    }

    bool MaterialSources::Has(const std::string& name) const
    {
        return Locate(name).has_value();
    }

    std::optional<materials::WawMaterial> MaterialSources::Find(const std::string& name) const
    {
        const auto located = Locate(name);
        if (!located)
            return std::nullopt;
        const auto& [root, defined] = *located;
        if (!root->fastfile)
            return ReadFrom(*root, defined);
        const auto key = Key(name);
        {
            const std::lock_guard guard(lock_);
            if (const auto found = read_.find(key); found != read_.end())
                return found->second;
        }
        auto material = ReadFrom(*root, defined);
        const std::lock_guard guard(lock_);
        return read_.emplace(key, std::move(material)).first->second;
    }

    void MaterialSources::Read(const std::vector<std::string>& names) const
    {
        std::map<const Root*, std::vector<std::pair<std::string, std::string>>> byZone; // key, defined name
        for (const auto& name : names)
            if (const auto located = Locate(name); located && located->first->fastfile)
                byZone[located->first].emplace_back(Key(name), located->second);
        for (const auto& root : roots_)
            if (const auto wanted = byZone.find(&root); wanted != byZone.end())
                for (const auto& [key, defined] : wanted->second)
                {
                    {
                        const std::lock_guard guard(lock_);
                        if (read_.contains(key))
                            continue;
                    }
                    auto material = ReadFrom(root, defined);
                    const std::lock_guard guard(lock_);
                    read_.emplace(key, std::move(material));
                }
    }

    std::set<std::string> MaterialSources::GeneratedNames() const
    {
        std::set<std::string> names;
        for (const auto& root : roots_)
        {
            if (root.fastfile)
            {
                for (const auto& [key, defined] : root.materials)
                    if (key.starts_with('*'))
                        names.insert(defined.substr(0, defined.find('(')));
                continue;
            }
            std::error_code e;
            for (const auto& entry : fs::directory_iterator(root.folder / "materials" / "generated", e))
            {
                const auto file = files::Utf8(entry.path().filename());
                auto lower = file;
                std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (file.starts_with('_') && lower.ends_with(".json"))
                    names.insert("*" + files::Utf8(entry.path().stem()).substr(1));
            }
        }
        return names;
    }
} // namespace w2b::extract
