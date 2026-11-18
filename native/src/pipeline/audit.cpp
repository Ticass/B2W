// Zone audit (see audit.h).
#include "audit.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/process.h"
#include "finish.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace w2b::pipeline
{
    namespace
    {
        std::string Str(fs::path path)
        {
            return files::Utf8(path.make_preferred());
        }

        std::string Posix(const fs::path& path)
        {
            return files::Utf8(path.generic_wstring());
        }

        py::Json LoadJson(const fs::path& path)
        {
            std::string bytes;
            files::ReadBytes(path, bytes);
            return py::Json::parse(bytes);
        }

        void SortPaths(std::vector<fs::path>& paths)
        {
            const auto key = [](const fs::path& p)
            {
                std::vector<std::wstring> parts;
                for (const auto& part : p)
                {
                    auto text = part.wstring();
                    for (auto& c : text)
                        if (c >= L'A' && c <= L'Z')
                            c = static_cast<wchar_t>(c - L'A' + L'a');
                    parts.push_back(text);
                }
                return parts;
            };
            std::stable_sort(paths.begin(), paths.end(), [&](const fs::path& a, const fs::path& b) { return key(a) < key(b); });
        }

        uint32_t EntryHash(const py::Json& entry)
        {
            if (entry.contains("nameHash"))
                return entry["nameHash"].get<uint32_t>();
            return NameHash(entry["name"].get<std::string>());
        }

        // str() of a JSON value inside a Python f-string
        std::string PyStr(const py::Json& value)
        {
            if (value.is_null())
                return "None";
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_boolean())
                return value.get<bool>() ? "True" : "False";
            if (value.is_number_float())
                return py::FloatRepr(value.get<double>());
            if (value.is_number())
                return value.dump();
            if (value.is_array())
            {
                std::string out = "[";
                for (size_t i = 0; i < value.size(); i++)
                    out += (i ? ", " : "") + PyStr(value[i]);
                return out + "]";
            }
            return value.dump();
        }

        bool IsClose(const double a, const double b)
        {
            return std::fabs(a - b) <= std::max(1e-6 * std::max(std::fabs(a), std::fabs(b)), 1e-6);
        }

        void Echo(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    } // namespace

    uint32_t NameHash(const std::string& name)
    {
        // T6 R_HashString: ORs 0x20 into every byte (differs from lower() on '_')
        uint32_t value = 0;
        for (const unsigned char byte : name)
            value = (value * 33) ^ (byte | 0x20u);
        return value;
    }

    MaterialAudit AuditMaterials(const fs::path& zone, const std::vector<fs::path>& extra)
    {
        std::map<std::string, fs::path> techsets;
        std::vector<fs::path> folders{zone};
        folders.insert(folders.end(), extra.begin(), extra.end());
        for (const auto& folder : folders)
        {
            const auto root = folder / "techniquesets";
            for (const auto& path : files::RGlob(root, ".json"))
            {
                auto relative = path.lexically_relative(root);
                techsets.emplace(Posix(relative.replace_extension()), path);
            }
        }
        MaterialAudit result;
        const auto materials = zone / "materials";
        auto paths = files::RGlob(materials, ".json");
        SortPaths(paths);
        std::map<std::string, py::Json> techsetCache;
        for (const auto& path : paths)
        {
            const auto material = LoadJson(path);
            auto relative = path.lexically_relative(materials);
            const auto name = Posix(relative.replace_extension());
            const auto ts = material.value("techniqueSet", py::Json());
            if (!ts.is_string() || ts.get<std::string>().empty())
                continue;
            const auto tsName = ts.get<std::string>();
            const auto found = techsets.find(tsName);
            if (found == techsets.end())
            {
                result.missingTechsets.insert(tsName);
                continue;
            }
            std::vector<uint32_t> constants, textures;
            for (const auto& c : material.value("constants", py::Json::array()))
                constants.push_back(EntryHash(c));
            for (const auto& t : material.value("textures", py::Json::array()))
                textures.push_back(EntryHash(t));
            auto cached = techsetCache.find(tsName);
            if (cached == techsetCache.end())
                cached = techsetCache.emplace(tsName, LoadJson(found->second)).first;
            const auto& techniques = cached->second["techniques"];
            for (size_t slot = 0; slot < techniques.size(); slot++)
            {
                const auto& technique = techniques[slot];
                const auto passes = technique.value("passArray", py::Json::array());
                for (size_t passIndex = 0; passIndex < passes.size(); passIndex++)
                {
                    const auto& pass = passes[passIndex];
                    const auto& args = pass["args"];
                    size_t cursor = 0;
                    for (const char* count : {"perPrimArgCount", "perObjArgCount", "stableArgCount"})
                    {
                        const size_t n = pass.value(count, 0);
                        const size_t end = std::min(args.size(), cursor + n);
                        std::map<int, size_t> positions{{0, 0}, {2, 0}, {6, 0}};
                        for (size_t a = cursor; a < end; a++)
                        {
                            const auto& arg = args[a];
                            const int type = arg["type"].get<int>();
                            if (type != 0 && type != 2 && type != 6)
                                continue;
                            result.checked++;
                            const auto& table = type == 2 ? textures : constants;
                            const auto wanted = arg["u"]["value"].get<uint32_t>();
                            std::optional<size_t> at;
                            for (size_t i = positions[type]; i < table.size(); i++)
                                if (table[i] == wanted)
                                {
                                    at = i;
                                    break;
                                }
                            if (!at)
                            {
                                char hex[16];
                                std::snprintf(hex, sizeof hex, "%08x", wanted);
                                const bool present = std::find(table.begin(), table.end(), wanted) != table.end();
                                result.problems.push_back(name + " [" + tsName + " slot " + std::to_string(slot) + " pass " + std::to_string(passIndex) + " " + count
                                                          + "] " + (type == 2 ? "texture" : "constant") + " hash 0x" + hex + " " + (present ? "out of order" : "absent"));
                            }
                            else
                                positions[type] = *at;
                        }
                        cursor += n;
                    }
                    if (cursor != args.size())
                        result.problems.push_back(name + " [" + tsName + " slot " + std::to_string(slot) + "] argument counts cover " + std::to_string(cursor) + " of "
                                                  + std::to_string(args.size()));
                }
            }
        }
        return result;
    }

    int PrintMaterialAudit(const fs::path& zone, const std::vector<fs::path>& extra)
    {
        const auto audit = AuditMaterials(zone, extra);
        for (const auto& line : audit.problems)
            std::printf("%s\n", line.c_str());
        for (const auto& name : audit.missingTechsets)
            std::printf("techset not found: %s\n", name.c_str());
        std::printf("%lld material arguments checked, %zu violations, %zu techsets not found\n", audit.checked, audit.problems.size(), audit.missingTechsets.size());
        std::fflush(stdout);
        return audit.problems.empty() && audit.missingTechsets.empty() ? 0 : 1;
    }

    bool HasAuthoredHitboxes(const fs::path& source)
    {
        for (const auto& path : files::RGlob(source / "xmodel", ".json"))
        {
            const auto model = LoadJson(path);
            const auto& info = model.value("boneInfo", py::Json());
            const auto type = model.value("type", py::Json());
            if (!info.is_null() && !info.empty() && type.is_string() && (type == "animated" || type == "viewhands"))
                return true;
        }
        return false;
    }

    py::Json AuditHitboxes(const fs::path& source, const fs::path& built)
    {
        long long models = 0, bones = 0;
        std::vector<std::string> errors;
        auto paths = files::RGlob(source / "xmodel", ".json");
        SortPaths(paths);
        for (const auto& path : paths)
        {
            const auto original = LoadJson(path);
            const auto& info = original.value("boneInfo", py::Json());
            const auto type = original.value("type", py::Json());
            if (info.is_null() || info.empty() || !type.is_string() || (type != "animated" && type != "viewhands"))
                continue;
            const auto target = built / path.lexically_relative(source);
            std::error_code ec;
            if (!fs::is_regular_file(target, ec))
                continue;
            models++;
            std::map<std::string, py::Json> actual;
            for (const auto& bone : LoadJson(target).value("boneInfo", py::Json::array()))
                actual[PyStr(bone["bone"])] = bone;
            const auto stem = files::Utf8(path.stem());
            for (const auto& expected : info)
            {
                const auto name = PyStr(expected["bone"]);
                const auto found = actual.find(name);
                if (found == actual.end())
                {
                    errors.push_back(stem + ": missing hitbox bone " + name);
                    continue;
                }
                bones++;
                const auto hitloc = expected.value("hitloc", py::Json());
                if (!hitloc.is_null() && found->second.value("hitloc", py::Json()) != hitloc)
                    errors.push_back(stem + ": " + name + " hit location changed: " + PyStr(hitloc) + " -> " + PyStr(found->second.value("hitloc", py::Json())));
                for (const char* key : {"mins", "maxs", "offset", "radiusSquared"})
                {
                    auto left = expected[key], right = found->second[key];
                    if (!left.is_array())
                        left = py::Json::array({left});
                    if (!right.is_array())
                        right = py::Json::array({right});
                    bool same = left.size() == right.size();
                    for (size_t i = 0; same && i < left.size(); i++)
                        same = IsClose(left[i].get<double>(), right[i].get<double>());
                    if (!same)
                        errors.push_back(stem + ": " + name + " " + key + " changed: " + PyStr(left) + " -> " + PyStr(right));
                }
            }
        }
        py::Json result = py::Json::object();
        result["models"] = models;
        result["bones"] = bones;
        result["errors"] = errors;
        return result;
    }

    void AuditZone(const fs::path& ff, const fs::path& stage, const std::string& project, const fs::path& bo2, const fs::path& stock, const fs::path& unlinker)
    {
        std::error_code ec;
        if (!fs::is_regular_file(ff, ec))
            throw StepError("FileNotFoundError", "Linker did not create " + Str(ff));
        const auto audit = stage / files::FromUtf8("audit_" + files::Utf8(ff.stem()));
        const auto relative = fs::weakly_canonical(audit, ec).lexically_relative(fs::weakly_canonical(stage, ec));
        if (relative.empty() || *relative.begin() == "..")
            throw StepError("ValueError", "Audit cleanup target is outside staging: " + Str(audit));
        fs::remove_all(audit, ec);
        const auto modelSource = stage / "zone_raw" / files::FromUtf8(project);
        const bool hitboxes = files::Utf8(ff.stem()) == project && HasAuthoredHitboxes(modelSource);
        const auto log = stage / files::FromUtf8(files::Utf8(audit.filename()) + ".log");
        // progress.native: the tool's log, streamed when verbose
        std::printf("[tool] %s; log: %s\n", files::Utf8(unlinker.filename()).c_str(), Str(log).c_str());
        std::fflush(stdout);
        const char* flag = std::getenv("WAW2BO2_VERBOSE");
        const auto proc = process::RunMerged({Str(unlinker), "--no-color", "--search-path", Str(bo2 / "zone/all"), "--include-assets",
                                              std::string("material,techniqueset") + (hitboxes ? ",xmodel" : ""), "--output-folder", Str(audit), Str(ff)},
                                             "", flag && std::string(flag) == "1" ? Echo : nullptr);
        files::WriteBytes(log, proc.out);
        if (proc.exitCode)
        {
            const auto text = encoding::Utf8Replace(proc.out);
            std::printf("%s\n", text.substr(text.size() > 8000 ? text.size() - 8000 : 0).c_str());
            throw StepError("RuntimeError", files::Utf8(unlinker.filename()) + " failed (" + std::to_string(static_cast<unsigned>(proc.exitCode)) + "); see " + Str(log));
        }
        if (const int code = PrintMaterialAudit(audit, {stock}))
            throw StepError("RuntimeError", "audit_material_args failed (" + std::to_string(code) + ")");
        if (hitboxes)
        {
            const auto result = AuditHitboxes(modelSource, audit);
            files::WriteText(stage / "bone_hitbox_audit.json", py::JsonDumps(result, 2));
            const auto& errors = result["errors"];
            if (!errors.empty() || result["models"].get<long long>() == 0)
            {
                std::string detail;
                for (size_t i = 0; i < std::min<size_t>(5, errors.size()); i++)
                    detail += (i ? "; " : "") + errors[i].get<std::string>();
                if (detail.empty())
                    detail = "no built model with authored hitboxes was found to audit";
                throw StepError("RuntimeError", "Authored bone hitboxes changed during linking (" + std::to_string(errors.size()) + " errors, "
                                                    + std::to_string(result["models"].get<long long>()) + " models): " + detail + "; see bone_hitbox_audit.json");
            }
            std::printf("%lld authored bone hitboxes checked across %lld models, 0 violations\n", result["bones"].get<long long>(), result["models"].get<long long>());
            std::fflush(stdout);
        }
    }
} // namespace w2b::pipeline
