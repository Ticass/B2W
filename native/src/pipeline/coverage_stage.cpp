// WaW framework coverage of a build (see coverage_stage.h).
#include "coverage_stage.h"

#include "core/encoding.h"
#include "core/pyre.h"
#include "extract/cache.h"
#include "gsc/parse.h"
#include "gsc/port/api.h"
#include "modutil.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::pipeline
{
    using mod::Lower;
    using mod::StartsWith;
    using py::Json;

    namespace
    {
        using Rows = std::vector<std::pair<std::string, std::string>>; // function -> status, sorted
        using Coverage = std::vector<std::pair<std::string, Rows>>;    // script key -> rows, framework_scripts order

        Coverage Sorted(Coverage cov)
        {
            std::sort(cov.begin(), cov.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            return cov;
        }

        // framework_scripts: maps\<script> -> its first copy along roots
        std::vector<std::pair<std::string, fs::path>> FrameworkScripts(const std::vector<fs::path>& roots)
        {
            static const pyre::Regex framework("^maps[\\\\/]_(zombiemode\\w*|laststand|gameskill)\\.gsc$", pyre::IGNORECASE);
            std::vector<std::pair<std::string, fs::path>> found;
            for (const auto& root : roots)
            {
                const auto folder = root / "maps";
                std::error_code e;
                if (!fs::is_directory(folder, e))
                    continue;
                std::vector<fs::path> paths;
                for (const auto& entry : fs::directory_iterator(folder, e))
                {
                    const auto name = Lower(files::Utf8(entry.path().filename()));
                    if (entry.is_regular_file(e) && StartsWith(name, "_") && mod::EndsWith(name, ".gsc"))
                        paths.push_back(entry.path());
                }
                std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return Lower(mod::Str(a)) < Lower(mod::Str(b)); });
                for (const auto& path : paths)
                {
                    const auto key = "maps\\" + Lower(files::Utf8(path.stem()));
                    if (framework.Search("maps/" + files::Utf8(path.filename())) &&
                        std::none_of(found.begin(), found.end(), [&](const auto& f) { return f.first == key; }))
                        found.emplace_back(key, path);
                }
            }
            return found;
        }

        Coverage Compute(const Json& scripts, const std::vector<fs::path>& roots)
        {
            static const pyre::Regex redirect("^(maps\\\\_\\w+)::(\\w+) -> (.+)$", pyre::IGNORECASE);
            std::set<std::string> extracted, ported;
            std::map<std::string, std::string> redirects;
            if (scripts.is_object())
            {
                for (const auto& e : scripts.value("extracted", Json::array()))
                {
                    const auto text = e.get<std::string>();
                    extracted.insert(Lower(text.substr(0, text.find(' '))));
                }
                for (const auto& p : scripts.value("ported", Json::array()))
                {
                    auto text = Lower(p.get<std::string>());
                    std::replace(text.begin(), text.end(), '/', '\\');
                    if (mod::EndsWith(text, ".gsc"))
                        text.resize(text.size() - 4);
                    ported.insert(text);
                }
                const auto rewrites = scripts.value("rewrites", Json::object());
                if (rewrites.is_object())
                    for (const auto& [rewrite, _] : rewrites.items())
                        if (const auto m = redirect.Search(rewrite))
                            redirects[Lower(*m->Group(1)) + "::" + Lower(*m->Group(2))] = *m->Group(3);
            }
            Coverage result;
            for (const auto& [key, path] : FrameworkScripts(roots))
            {
                const auto script = gsc::Parse(encoding::UniversalNewlines(encoding::Utf8Replace(mod::Bytes(path))), key);
                const auto base = key.substr(key.rfind('\\') + 1);
                const bool whole = std::any_of(ported.begin(), ported.end(), [&](const std::string& p) { return mod::EndsWith(p, base); });
                std::vector<std::string> names;
                for (const auto& [name, _] : script.functions)
                    names.push_back(name);
                std::sort(names.begin(), names.end());
                Rows rows;
                for (const auto& name : names)
                {
                    const auto qualified = key + "::" + name;
                    if (whole || extracted.count(qualified))
                        rows.emplace_back(name, "ported");
                    else if (const auto it = redirects.find(qualified); it != redirects.end())
                        rows.emplace_back(name, it->second.find("compat") != std::string::npos ? std::string("compat") : "bo2 (" + it->second + ")");
                    else
                        rows.emplace_back(name, "not run");
                }
                result.emplace_back(key, rows);
            }
            return result;
        }

        std::string Head(const std::string& status)
        {
            return status.substr(0, status.find(' '));
        }

        // Counter over the first words, in first-seen order
        std::vector<std::pair<std::string, long long>> Count(const Coverage& cov, const std::string* only = nullptr)
        {
            std::vector<std::pair<std::string, long long>> out;
            for (const auto& [key, rows] : cov)
            {
                if (only && key != *only)
                    continue;
                for (const auto& [_, status] : rows)
                {
                    const auto h = Head(status);
                    auto it = std::find_if(out.begin(), out.end(), [&](const auto& c) { return c.first == h; });
                    if (it == out.end())
                        out.emplace_back(h, 1);
                    else
                        it->second++;
                }
            }
            return out;
        }

        std::string Markdown(const std::string& project, const Coverage& cov)
        {
            std::string out = "# WaW framework coverage: " + project + "\n\n";
            out += "| status | functions |\n|---|---|\n";
            auto totals = Count(cov);
            std::sort(totals.begin(), totals.end());
            for (const auto& [k, v] : totals)
                out += "| " + k + " | " + std::to_string(v) + " |\n";
            for (const auto& [key, rows] : Sorted(cov))
            {
                auto counts = Count(cov, &key);
                std::sort(counts.begin(), counts.end());
                std::string summary;
                for (const auto& [k, v] : counts)
                    summary += (summary.empty() ? "" : ", ") + k + " " + std::to_string(v);
                out += "\n## " + key + " (" + summary + ")\n\n| function | in the port |\n|---|---|\n";
                for (const auto& [name, status] : rows)
                    out += "| " + name + " | " + status + " |\n";
            }
            out += "\n## Opt-in fixes (compat/waw_fixes.json, off by default)\n\n| console dvar | changes from WaW |\n|---|---|\n";
            const auto fixes = mod::Json(extract::ResourceRoot() / "src/waw2bo2/compat/waw_fixes.json");
            for (const auto& [name, fix] : fixes["fixes"].items())
            {
                const auto& d = fix["default"];
                const auto value = d.is_string() ? d.get<std::string>() : d.is_boolean() ? (d.get<bool>() ? "True" : "False") : d.is_number_integer() ? std::to_string(d.get<long long>()) : py::FloatRepr(d.get<double>());
                out += "| " + name + " " + value + " | " + fix["changes"].get<std::string>() + " |\n";
            }
            return out;
        }
    } // namespace

    Json FrameworkCoverage(const fs::path& requestPath)
    {
        const auto request = mod::Json(requestPath);
        const auto project = request["project"].get<std::string>();
        const auto projectRoot = files::FromUtf8(request["project_root"].get<std::string>());
        std::vector<fs::path> roots;
        for (const auto& r : request["roots"])
            roots.push_back(files::FromUtf8(r.get<std::string>()));
        const auto cov = Compute(request.value("scripts", Json()), roots);
        gsc::port::SetCompatDir(extract::ResourceRoot() / "src/waw2bo2/compat");
        const auto& api = gsc::port::Api();
        Json warnings = Json::array();
        for (const auto& [key, rows] : Sorted(cov))
            for (const auto& [name, status] : rows)
            {
                if (!StartsWith(status, "bo2"))
                    continue;
                bool documented = false;
                if (api.contains("source_driven") && api["source_driven"].is_object())
                    for (const auto& [script, driven] : api["source_driven"].items())
                        if (Lower(script) == Lower(key))
                        {
                            const auto glue = driven.value("glue", Json::object());
                            documented = glue.contains(name) && !glue[name].is_null() && !(glue[name].is_string() && glue[name].get<std::string>().empty());
                            break;
                        }
                if (!documented)
                    warnings.push_back("FRAMEWORK_BO2 " + key + "::" + name + ": " + status + "; the map's own function does not run");
            }
        Json document = Json::object();
        for (const auto& [key, rows] : cov)
        {
            Json r = Json::object();
            for (const auto& [name, status] : rows)
                r[name] = status;
            document[key] = r;
        }
        files::WriteText(projectRoot / "framework_coverage.json", py::JsonDumps(document, 1) + "\n");
        files::WriteText(projectRoot / "framework_coverage.md", Markdown(project, cov));
        Json totals = Json::object();
        for (const auto& [k, v] : Count(cov))
            totals[k] = v;
        return Json{{"totals", totals}, {"warnings", warnings}};
    }
} // namespace w2b::pipeline
