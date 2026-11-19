// t6bridge.stage_materials, the per-material part: WaW materials converted
// with their translated programs (_stage_waw_material), BO2 native
// equivalents (_stage_native_material), applied in name order.
#include "materials_stage.h"

#include "core/profile.h"
#include "core/asset_paths.h"
#include "core/hash.h"
#include "core/parallel.h"
#include "extract/cache.h"
#include "extract/material_sources.h"
#include "materials/prepare.h"
#include "modutil.h"
#include "shaders/runtime.h"
#include "textures/stage.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::Bytes;
    using mod::Copy;
    using mod::Generic;
    using mod::Lower;
    using mod::StartsWith;
    using mod::Str;
    using mod::Truthy;
    using py::Json;

    namespace
    {
        Json ReadJson(const fs::path& p)
        {
            return mod::Json(p);
        }
    } // namespace

    namespace
    {
        // t6bridge._find: the first root holding the path (file or folder)
        std::optional<fs::path> FindAny(const std::vector<fs::path>& roots, const fs::path& relative)
        {
            std::error_code e;
            for (const auto& r : roots)
                if (fs::exists(r / relative, e))
                    return r / relative;
            return std::nullopt;
        }

        std::vector<std::string> StarParts(const std::string& name)
        {
            const auto body = name.substr(1, name.find('(') == std::string::npos ? std::string::npos : name.find('(') - 1);
            std::vector<std::string> out;
            for (const auto part : py::Split(body, "_"))
                out.emplace_back(part);
            return out;
        }

        // dangling_substitute: a defined material for a reference no WaW zone defines
        std::optional<std::pair<std::string, std::string>> DanglingSubstitute(const std::string& name, const extract::MaterialSources& sources)
        {
            if (StartsWith(name, "*"))
            {
                const auto want = StarParts(name);
                std::optional<std::pair<std::tuple<int, int, int, std::string>, std::string>> best;
                for (const auto& generated : sources.GeneratedNames())
                {
                    const auto body = generated.substr(1), stem = "_" + body;
                    std::vector<std::string> parts;
                    for (const auto part : py::Split(body, "_"))
                        parts.emplace_back(part);
                    int common = 0;
                    for (size_t i = 0; i < std::min(want.size(), parts.size()); i++)
                    {
                        if (want[i] != parts[i])
                            break;
                        common++;
                    }
                    const std::set<std::string> a(want.begin(), want.end()), b(parts.begin(), parts.end());
                    int shared = 0;
                    for (const auto& x : a)
                        shared += b.count(x) ? 1 : 0;
                    if (!shared)
                        continue;
                    const auto key = std::tuple{common, shared, -std::abs(static_cast<int>(parts.size()) - static_cast<int>(want.size())), stem};
                    if (!best || key > best->first)
                        best = std::pair{key, generated};
                }
                if (best)
                    return std::pair{best->second, "dangling reference " + name + ": WaW draws its default material; using blend " + best->second};
                return std::nullopt;
            }
            const std::string fallback = "mc/mtl_default";
            if (sources.Has(fallback))
                return std::pair{fallback, "dangling reference " + name + ": WaW draws its default material; using " + fallback};
            return std::nullopt;
        }

        std::string IntList(std::vector<int> values)
        {
            std::string out = "[";
            for (size_t i = 0; i < values.size(); i++)
                out += (i ? ", " : "") + std::to_string(values[i]);
            return out + "]";
        }

        struct Context
        {
            std::vector<fs::path> roots;
            std::optional<extract::MaterialSources> sources;
            fs::path stockMaterials, projectRoot;
            std::optional<fs::path> techsetRoot;
            std::map<std::string, std::string> rename;
            std::string imagePrefix;
            Json donorPaths;
            std::optional<std::array<double, 4>> falloff;
            bool lightShadows = true, layered = false;
            std::set<std::string> native, nativeFallbacks;
            std::map<std::string, fs::path> fallbacks;
        };

        std::string Renamed(const Context& c, const std::string& name)
        {
            const auto it = c.rename.find(name);
            return it == c.rename.end() ? name : it->second;
        }

        struct Result
        {
            bool native = false;
            std::optional<MaterialRecord> entry; // none: no material
            std::string techset;
            Json warnings = Json::array(), errors = Json::array(), images = Json::array();
            std::vector<std::pair<std::string, std::string>> files; // (relative, bytes), Path order
        };

        // one WaW material: its files stay in memory until the caller writes them in name order
        Result StageWaw(const Context& c, materials::Donors& donors, const std::string& name)
        {
            Result result;
            shaders::MemoryOutput output;
            const auto rel = assets::MaterialPath(name);
            auto source = c.sources->Find(name);
            std::optional<std::pair<std::string, std::string>> substitute;
            if (!source)
                if ((substitute = DanglingSubstitute(name, *c.sources)))
                    source = c.sources->Find(substitute->first);
            const auto outRel = assets::MaterialPath(Renamed(c, name));
            MaterialRecord entry;
            entry.name = Renamed(c, name);
            entry.file = Generic(outRel);
            if (c.rename.count(name))
                entry.source = name;
            std::vector<std::string> notes;
            if (!source)
            {
                result.errors.push_back("material " + name + ": no WaW source " + Generic(rel) + " in any zone dump");
                return result;
            }
            if (substitute)
            {
                entry.substitutedFrom = substitute->first;
                notes.push_back(substitute->second);
                result.warnings.push_back(substitute->second);
            }
            const auto& t4 = *source;
            const auto& t4ts = t4.techniqueSet;
            entry.t4Techset = std::optional(t4ts);
            entry.t4SortKey = t4.sortKey;
            std::error_code e;
            bool programs = false;
            if (c.techsetRoot)
                for (const auto& root : c.roots)
                    programs |= fs::exists(root / "waw_techniquesets" / files::FromUtf8(t4ts + ".json"), e);
            auto prepared = [&] { const profile::Scope scope("materials: select donor"); return materials::SelectMaterial(t4, programs, c.layered, donors); }();
            if (prepared.error)
            {
                result.errors.push_back("material " + name + " (" + t4ts + "): " + *prepared.error);
                return result;
            }
            const auto& target = prepared.target;
            auto& out = prepared.material;
            for (const auto& n : prepared.notes)
                notes.push_back(n);
            if (prepared.approximation && !prepared.approximation->empty())
                result.warnings.push_back("APPROXIMATED_MATERIAL " + name + ": no T6 rule for WaW technique '" + t4ts + "'; drawn with its own textures as '" +
                                          *prepared.approximation + "' (" + target + ")");
            Json runtime = c.techsetRoot ? shaders::BindMaterial(t4, out, c.roots, output, *c.techsetRoot, c.falloff, c.lightShadows)
                                         : Json{{"active", Json::array()}, {"unsupported", Json::array({"native technique dump absent"})}};
            bool sourceProgram = false;
            for (const auto& n : prepared.matchNotes)
                sourceProgram |= n.find("source program supplies") != std::string::npos;
            if (sourceProgram)
            {
                std::set<int> active;
                for (const auto& a : runtime.value("active", Json::array()))
                    active.insert(a["slot"].get<int>());
                std::vector<int> missing;
                for (const int slot : {4, 5, 6, 7, 8, 13, 14})
                    if (!active.count(slot))
                        missing.push_back(slot);
                if (!missing.empty())
                    result.errors.push_back("material " + name + ": layered stream donor requires original WaW lit programs; missing slots " + IntList(missing));
            }
            if (t4ts == "wc_unlit_distfalloff")
            {
                bool paired = true;
                for (const int slot : {2, 3})
                {
                    bool found = false;
                    for (const auto& p : runtime.value("active", Json::array()))
                        found |= p.value("slot", -1) == slot && Truthy(p, "paired");
                    paired &= found;
                }
                if (paired)
                {
                    std::erase(notes, std::string("distance falloff dropped"));
                }
            }
            entry.shaderRuntime = runtime;
            const auto stateSlots = shaders::ApplySourcePassStates(t4, out, runtime);
            if (!stateSlots.empty())
                notes.push_back("WaW pass blend/depth/raster state on T6 slots " + IntList(stateSlots));
            for (auto& texture : out.textures)
            {
                const auto& image = texture.image;
                if (!c.imagePrefix.empty() && !image.empty() && !StartsWith(image, ",") && !StartsWith(image, "$") && !StartsWith(image, c.imagePrefix))
                    texture.image = c.imagePrefix + image;
            }
            entry.t6Techset = out.techniqueSet;
            entry.donor = Generic(fs::path(files::FromUtf8(c.donorPaths[target].get<std::string>())).lexically_relative(c.stockMaterials));
            entry.cost = prepared.cost;
            entry.notes = std::move(notes);
            output.Write(outRel, materials::T6MaterialText(out), true);
            result.entry = entry;
            result.techset = out.techniqueSet;
            result.files = output.Files();
            return result;
        }

        Result StageNative(const Context& c, const std::string& name)
        {
            // the BO2 equivalent of a material absent from the whole WaW lookup
            Result result;
            result.native = true;
            const auto outRel = assets::MaterialPath(Renamed(c, name));
            MaterialRecord entry;
            entry.name = Renamed(c, name);
            entry.file = Generic(outRel);
            if (c.rename.count(name))
                entry.source = name;
            std::vector<std::string> notes;
            const auto& nativePath = c.fallbacks.at(name);
            auto out = materials::ReadT6Material(nativePath);
            if (out.thermalMaterial)
            {
                if (!out.thermalMaterial->empty())
                    notes.push_back("BO2 thermalMaterial dropped (the WaW material has none)");
                out.thermalMaterial.reset();
            }
            const auto nativeRoot = c.stockMaterials.parent_path();
            for (auto& texture : out.textures)
                {
                    auto image = texture.image;
                    if (StartsWith(image, ","))
                        image.erase(0, 1);
                    if (image.empty() || StartsWith(image, "$"))
                    {
                        texture.image = StartsWith(image, "$") ? "," + image : image;
                        continue;
                    }
                    const auto output = (c.imagePrefix.empty() ? std::string("waw_image/") : c.imagePrefix) + "bo2_fallback/" + image;
                    fs::path materialFolder;
                    for (auto p = nativePath.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
                        if (files::Utf8(p.filename()) == "materials")
                        {
                            materialFolder = p;
                            break;
                        }
                    const std::vector<fs::path> imageRoots{nativeRoot, materialFolder.parent_path()};
                    auto imagePath = FindAny(imageRoots, assets::ImagePath(image, ".iwi"));
                    if (!imagePath && !FindAny(imageRoots, assets::ImagePath(image, ".dds")))
                        imagePath = extract::RequiredBo2Image(nativeRoot, image);
                    const auto dst = c.projectRoot / assets::ImagePath(output, ".iwi");
                    fs::create_directories(dst.parent_path());
                    if (imagePath)
                    {
                        if (Bytes(*imagePath).substr(0, 4) != std::string("IWi\x1b", 4))
                            throw StepError("StageError", "BO2 equivalent image " + image + ": not an IWI27 source");
                        Copy(*imagePath, dst);
                    }
                    else
                    {
                        imagePath = FindAny(imageRoots, assets::ImagePath(image, ".dds"));
                        if (!imagePath)
                            throw StepError("StageError", "BO2 equivalent material " + name + ": image " + image + " missing from " + Str(nativeRoot));
                        textures::ConvertFile(*imagePath, dst);
                    }
                    texture.image = output;
                    result.images.push_back(Json{{"name", output}, {"source", Str(*imagePath)}, {"source_kind", "t6_equivalent"}});
                }
            const auto dst = c.projectRoot / outRel;
            fs::create_directories(dst.parent_path());
            files::WriteText(dst, materials::T6MaterialText(out));
            const std::string why = "original material absent from WaW lookup";
            entry.t4Techset = std::optional<std::string>();
            entry.t6Techset = out.techniqueSet;
            entry.nativeEquivalent = Str(nativePath);
            notes.insert(notes.begin(), why);
            entry.notes = std::move(notes);
            result.entry = entry;
            result.techset = out.techniqueSet;
            result.warnings.push_back("BO2_FALLBACK material " + name + " -> " + Str(nativePath) + ": " + why);
            return result;
        }
    } // namespace

    std::vector<MaterialOutcome> StageMaterials(const MaterialStageRequest& request)
    {
        Context c;
        c.roots = request.roots;
        c.stockMaterials = request.stockMaterials;
        c.projectRoot = request.projectRoot;
        c.techsetRoot = request.techsetRoot;
        c.imagePrefix = request.imagePrefix;
        c.donorPaths = request.donors;
        c.falloff = request.falloffPlacement;
        c.lightShadows = request.lightShadows;
        c.layered = request.layeredVertexData;
        c.fallbacks = request.nativeFallbacks;
        c.rename = request.rename;
        std::vector<std::string> names(request.names.begin(), request.names.end());
        std::sort(names.begin(), names.end());
        materials::Donors donors(c.donorPaths);
        c.sources.emplace(c.roots);
        {
            const profile::Scope scope("materials: read WaW sources");
            c.sources->Read(names);
        }
        // WaW materials convert independently; native equivalents stay in order below
        std::vector<std::optional<Result>> results(names.size());
        std::vector<bool> nativeName(names.size());
        for (size_t i = 0; i < names.size(); i++)
            nativeName[i] = !c.sources->Has(names[i]) && c.fallbacks.count(names[i]);
        parallel::For(names.size(), [&](const size_t i)
                      {
                          if (!nativeName[i])
                              results[i] = StageWaw(c, donors, names[i]);
                      });
        std::vector<MaterialOutcome> out;
        std::map<std::string, std::string> written;
        for (size_t i = 0; i < names.size(); i++)
        {
            auto result = nativeName[i] ? StageNative(c, names[i]) : std::move(*results[i]);
            for (const auto& [relative, data] : result.files)
            {
                const auto dst = c.projectRoot / files::FromUtf8(relative);
                const auto digest = hash::Sha256Hex(data);
                const auto key = Lower(Str(dst));
                if (written.count(key) && written[key] == digest)
                    continue;
                fs::create_directories(dst.parent_path());
                files::WriteBytes(dst, data);
                written[key] = digest;
            }
            out.push_back({names[i], result.native, std::move(result.entry), std::move(result.techset), std::move(result.warnings),
                           std::move(result.errors), std::move(result.images)});
        }
        return out;
    }
} // namespace w2b::pipeline
