// Materials the shader relink of mod.ff rebinds: the source map's impact
// marks (impactmaterials.bind) and stock readers of the converted probe cubes
// (nativeprobes.bind_native_materials).
#include "shaderbind.h"

#include "core/hash.h"
#include "core/profile.h"
#include "core/pyre.h"
#include "modutil.h"
#include "shaders/runtime.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using namespace mod;

    namespace
    {
        // gfx_impact_* materials of mc/ and wc/ under a root's materials folder
        void ImpactNames(const fs::path& root, std::set<std::string>& names)
        {
            for (const std::string family : {"mc", "wc"})
            {
                std::error_code e;
                const auto folder = root / "materials" / family;
                if (!fs::is_directory(folder, e))
                    continue;
                for (const auto& entry : fs::directory_iterator(folder, e))
                {
                    const auto file = files::Utf8(entry.path().filename());
                    if (StartsWith(Lower(file), "gfx_impact_") && EndsWith(Lower(file), ".json"))
                        names.insert(family + "/" + file.substr(0, file.size() - 5));
                }
            }
        }
    } // namespace

    std::map<std::string, std::string> BindImpactMaterials(const fs::path& project, const fs::path& dump, const fs::path& overlay, const fs::path& stock)
    {
        // route matching stock mark identities to the original WaW materials;
        // T6's <name>_blend is the engine's blend variant of <name>
        const auto root = project / "content_source/impact_materials";
        const auto manifest = root / "stage.json";
        if (!fs::exists(manifest))
            return {};
        std::map<std::string, py::Json> entries;
        for (const auto& m : Member(manifest, "materials"))
            if (m.contains("shader_runtime") && Truthy(m["shader_runtime"], "active"))
                entries[m.contains("source") ? m["source"].get<std::string>() : m["name"].get<std::string>()] = m;
        std::set<std::string> candidates;
        ImpactNames(dump, candidates);
        ImpactNames(stock, candidates);
        std::map<std::string, std::string> changed;
        for (const auto& name : candidates)
        {
            auto original = name;
            if (!entries.count(original) && EndsWith(original, "_blend"))
                original = original.substr(0, original.size() - 6);
            if (!entries.count(original))
                continue;
            const auto material = Json(root / files::FromUtf8(entries[original]["file"].get<std::string>()));
            const auto relative = "materials/" + name + ".json";
            const auto target = overlay / files::FromUtf8(relative);
            MakeParent(target);
            files::WriteText(target, py::JsonDumpsDefault(material));
            for (const auto& texture : material.value("textures", py::Json::array()))
            {
                const auto image = texture.value("image", std::string());
                if (image.empty() || image[0] == ',' || image[0] == '$')
                    continue;
                const auto destination = overlay / "images" / files::FromUtf8(image + ".iwi");
                MakeParent(destination);
                Copy(root / "images" / files::FromUtf8(image + ".iwi"), destination);
            }
            changed[relative] = original;
        }
        if (!changed.empty())
            for (const std::string folder : {"techniquesets", "shader_bin"})
                CopyTree(root / folder, overlay / folder);
        return changed;
    }

    std::map<std::string, int> BindNativeProbeMaterials(const fs::path& dump, const fs::path& overlay, const fs::path& stock)
    {
        // stock gameplay readers of the converted world's probe cubes; the
        // translated overlay's own materials stay authoritative
        std::vector<std::pair<std::string, fs::path>> sources;
        for (const auto& source : files::RGlob(dump / "materials", ".json"))
            sources.emplace_back(Generic(source.lexically_relative(dump)), source);
        // sorted(Path): case-insensitive on Windows
        std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return Lower(Str(a.second)) < Lower(Str(b.second)); });
        std::map<std::string, int> changed;
        shaders::FolderOutput overlayOutput(overlay);
        for (const auto& [relative, source] : sources)
        {
            const auto target = overlay / files::FromUtf8(relative);
            if (fs::exists(target))
                continue;
            auto material = [&] { const profile::Scope scope("probes: read material"); return materials::ReadT6Material(source); }();
            if (const int count = shaders::BindNativeProbes(material, overlayOutput, stock))
            {
                MakeParent(target);
                files::WriteText(target, py::JsonDumpsDefault(materials::ToJson(material)));
                changed[relative] = count;
            }
        }
        return changed;
    }
} // namespace w2b::pipeline
