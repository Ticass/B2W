// Knowledge ledger (see knowledge.h).
#include "knowledge.h"

#include "core/files.h"
#include "core/encoding.h"
#include "core/pyre.h"
#include "core/zip.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace w2b::pipeline::knowledge
{
    namespace
    {
        std::string Sha1Hex(const std::string& data)
        {
            unsigned char digest[20] = {};
            BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), digest, 20);
            static const char* hex = "0123456789abcdef";
            std::string out;
            for (const unsigned char b : digest)
                out += hex[b >> 4], out += hex[b & 15];
            return out;
        }

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        const pyre::Regex& Tag()
        {
            static const pyre::Regex tag(R"(^([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)\b)");
            return tag;
        }

        // str.islower(): a cased character and no uppercase one
        bool IsLower(const std::string& word)
        {
            bool cased = false;
            for (const char c : word)
            {
                if (c >= 'A' && c <= 'Z')
                    return false;
                cased = cased || (c >= 'a' && c <= 'z');
            }
            return cased;
        }

        // Counter(...).most_common(): by count, ties in first-seen order
        py::Json MostCommon(const std::vector<std::string>& items)
        {
            std::vector<std::pair<std::string, long long>> counts;
            for (const auto& item : items)
            {
                auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == item; });
                if (it == counts.end())
                    counts.emplace_back(item, 1);
                else
                    it->second++;
            }
            std::stable_sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            py::Json out = py::Json::object();
            for (const auto& [k, v] : counts)
                out[k] = v;
            return out;
        }
    } // namespace

    std::pair<std::string, std::string> Classify(const std::string& entry)
    {
        const auto space = entry.find(' ');
        const auto head = space == std::string::npos ? entry : entry.substr(0, space);
        const auto rest = space == std::string::npos ? std::string() : entry.substr(space + 1);
        if (head == "GSC")
        {
            static const pyre::Regex arity(R"(^(?P<name>\w+)/(?P<argc>\d+)args\((?P<why>.*)\)$)");
            if (const auto m = arity.MatchAt(rest))
                return {"gsc_arity", *m->Group("name") + "/" + *m->Group("argc")};
            return {rest.find("::") != std::string::npos ? "gsc_framework" : "gsc_builtin", Lower(rest)};
        }
        if (head == "XANIM")
        {
            static const pyre::Regex xanim(R"(^(?P<name>\S+) \(animtree (?P<tree>[^:]+): (?P<why>.*)\)$)");
            if (const auto m = xanim.MatchAt(rest))
                return {"xanim_missing", *m->Group("tree") + "::" + *m->Group("name")};
            return {"xanim", rest};
        }
        if (head == "CSC")
        {
            static const pyre::Regex csc(R"(^(?P<name>\S+) in client listener (?P<listener>\S+))");
            const auto m = csc.MatchAt(rest);
            return {"csc_builtin", m ? Lower(*m->Group("name")) : rest};
        }
        return {head.empty() ? "other" : Lower(head), rest};
    }

    std::string ErrorKind(const std::string& text)
    {
        if (const auto m = Tag().MatchAt(text))
            return (*m)[1];
        const auto colon = text.find(':');
        const auto head = std::string(py::Strip(colon == std::string::npos ? text : text.substr(0, colon)));
        static const pyre::Regex spaces(R"(\s+)");
        static const pyre::Regex stop(R"([\\/.]|\d)");
        std::vector<std::string> words;
        for (const auto& word : spaces.Split(head))
        {
            if (stop.Search(word) || (!words.empty() && IsLower(word) && word.find('_') != std::string::npos))
                break;
            words.push_back(Lower(word));
            if (words.size() == 3)
                break;
        }
        std::string out;
        for (size_t i = 0; i < words.size(); i++)
            out += (i ? " " : "") + words[i];
        return out.empty() ? "other" : out;
    }

    std::string MapKey(const std::string& source)
    {
        auto path = files::FromUtf8(source);
        const auto text = files::Utf8(path.make_preferred());
        return Lower(files::Utf8(path.stem())) + "_" + Sha1Hex(Lower(text)).substr(0, 8);
    }

    py::Json Observation(const py::Json* reportIn, const std::string& source, const std::string& project, const std::string& version, const std::string& outcome,
                         const std::optional<std::string>& step, const std::string& error)
    {
        const py::Json report = reportIn && reportIn->is_object() ? *reportIn : py::Json::object();
        const auto scripts = report.contains("scripts") && report["scripts"].is_object() ? report["scripts"] : py::Json::object();
        std::map<std::string, std::map<std::string, long long>> gaps;
        if (scripts.contains("unsupported") && scripts["unsupported"].is_object())
            for (const auto& [entry, where] : scripts["unsupported"].items())
            {
                const auto [kind, symbol] = Classify(entry);
                gaps[kind][symbol] += std::max<long long>(1, where.is_array() ? static_cast<long long>(where.size()) : 0);
            }
        std::vector<std::string> errorKinds, tags;
        for (const char* key : {"errors"})
            for (const auto& e : report.value(key, py::Json::array()))
                errorKinds.push_back(ErrorKind(e.get<std::string>()));
        for (const auto& e : scripts.value("errors", py::Json::array()))
            errorKinds.push_back(ErrorKind(e.get<std::string>()));
        for (const auto& w : report.value("warnings", py::Json::array()))
            if (const auto m = Tag().MatchAt(w.get<std::string>()))
                tags.push_back((*m)[1]);
        py::Json gapJson = py::Json::object();
        for (const auto& [kind, symbols] : gaps)
        {
            py::Json s = py::Json::object();
            for (const auto& [symbol, uses] : symbols)
                s[symbol] = uses;
            gapJson[kind] = s;
        }
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char date[16];
        std::strftime(date, sizeof date, "%Y-%m-%d", &local);
        py::Json obs = py::Json::object();
        obs["schema"] = SCHEMA;
        obs["map"] = MapKey(source);
        obs["source"] = files::Utf8(files::FromUtf8(source).filename());
        obs["project"] = !project.empty() ? project : report.value("project", std::string());
        obs["converter_version"] = version;
        obs["recorded"] = date;
        obs["outcome"] = outcome;
        obs["failed_step"] = step ? py::Json(*step) : py::Json();
        obs["error"] = error.size() > 2000 ? error.substr(error.size() - 2000) : error;
        obs["gaps"] = gapJson;
        obs["stage_errors"] = MostCommon(errorKinds);
        obs["reported"] = MostCommon(tags);
        obs["scripts_ported"] = scripts.value("ported", py::Json::array()).size();
        obs["functions_extracted"] = scripts.value("extracted", py::Json::array()).size();
        return obs;
    }

    std::filesystem::path Record(const std::filesystem::path& ledger, const py::Json& observation)
    {
        std::filesystem::create_directories(ledger);
        const auto path = ledger / files::FromUtf8(observation["map"].get<std::string>() + ".json");
        files::WriteText(path, py::JsonDumps(observation, 1) + "\n");
        return path;
    }

    std::optional<std::string> FailedStep(const std::string& log)
    {
        static const pyre::Regex banner(R"(^== (\d+[a-z]?)\. (.*)$)", pyre::MULTILINE);
        const auto steps = banner.FindAll(log);
        if (steps.empty())
            return std::nullopt;
        return steps.back()[0] + ". " + steps.back()[1];
    }

    std::vector<py::Json> Load(const std::vector<std::filesystem::path>& ledgers)
    {
        std::map<std::string, py::Json> found;
        std::vector<std::string> order;
        for (const auto& ledger : ledgers)
        {
            auto paths = files::GlobSuffix(ledger, ".json");
            std::ranges::sort(paths);
            for (const auto& path : paths)
            {
                py::Json obs;
                try
                {
                    obs = py::Json::parse(files::ReadText(path));
                }
                catch (const std::exception&)
                {
                    continue;
                }
                if (obs.value("schema", 0) != SCHEMA)
                    continue;
                const auto map = obs.at("map").get<std::string>();
                const auto old = found.find(map);
                if (old == found.end())
                    order.push_back(map);
                if (old == found.end() || obs.value("recorded", std::string()) >= old->second.value("recorded", std::string()))
                    found[map] = std::move(obs);
            }
        }
        std::vector<py::Json> out;
        for (const auto& map : order)
            out.push_back(std::move(found[map]));
        return out;
    }

    py::Json Aggregate(const std::vector<py::Json>& observations)
    {
        struct Gap
        {
            std::string kind, symbol;
            std::vector<std::string> maps;
            long long uses = 0;
        };
        std::map<std::pair<std::string, std::string>, Gap> gaps;
        std::map<std::string, std::set<std::string>> errors, steps;
        py::Json outcomes = py::Json::object();
        for (const auto& obs : observations)
        {
            const auto source = obs.value("source", std::string());
            const auto name = source.empty() ? obs.at("map").get<std::string>() : source;
            const auto outcome = obs.value("outcome", std::string("unknown"));
            outcomes[outcome] = outcomes.value(outcome, 0) + 1;
            const auto gapKinds = obs.value("gaps", py::Json::object());
            for (const auto& [kind, symbols] : gapKinds.items())
                for (const auto& [symbol, uses] : symbols.items())
                {
                    auto& gap = gaps.try_emplace({kind, symbol}, Gap{kind, symbol}).first->second;
                    gap.maps.push_back(name);
                    gap.uses += uses.get<long long>();
                }
            const auto stageErrors = obs.value("stage_errors", py::Json::object());
            for (const auto& [kind, _] : stageErrors.items())
                errors[kind].insert(name);
            if (const auto step = obs.value("failed_step", py::Json()); step.is_string() && !step.get<std::string>().empty())
                steps[step.get<std::string>()].insert(name);
        }
        std::vector<Gap> ranked;
        for (auto& [_, gap] : gaps)
            ranked.push_back(std::move(gap));
        std::ranges::sort(ranked, [](const Gap& a, const Gap& b)
                          { return std::tuple(-static_cast<long long>(a.maps.size()), -a.uses, a.kind, a.symbol) <
                                   std::tuple(-static_cast<long long>(b.maps.size()), -b.uses, b.kind, b.symbol); });
        py::Json gapList = py::Json::array();
        for (auto& gap : ranked)
        {
            std::ranges::sort(gap.maps);
            gapList.push_back({{"kind", gap.kind}, {"symbol", gap.symbol}, {"maps", gap.maps}, {"uses", gap.uses}});
        }
        // by the number of maps, then by name
        const auto byMaps = [](const std::map<std::string, std::set<std::string>>& groups)
        {
            std::vector<std::pair<std::string, std::set<std::string>>> items(groups.begin(), groups.end());
            std::ranges::stable_sort(items, [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
            py::Json out = py::Json::object();
            for (const auto& [key, maps] : items)
                out[key] = std::vector<std::string>(maps.begin(), maps.end());
            return out;
        };
        return {{"schema", SCHEMA},
                {"maps", observations.size()},
                {"outcomes", outcomes},
                {"gaps", gapList},
                {"stage_errors", byMaps(errors)},
                {"failed_steps", byMaps(steps)}};
    }

    std::optional<py::Json> FromBundle(const std::filesystem::path& bundle)
    {
        zip::Archive archive(bundle);
        py::Json summary = py::Json::object(), report;
        std::string console;
        bool haveReport = false;
        for (const auto& entry : archive.Entries())
        {
            if (entry.name == "summary.json")
                summary = py::Json::parse(archive.Read(entry));
            else if (entry.name == "console.log")
                console = encoding::Utf8Replace(archive.Read(entry));
            else if (!haveReport && entry.name.ends_with(std::string("/") + REPORT_NAME))
            {
                report = py::Json::parse(archive.Read(entry));
                haveReport = true;
            }
        }
        const auto settings = summary.value("settings", py::Json::object());
        auto source = settings.value("fastfile", std::string());
        if (source.empty())
            source = settings.value("project", std::string());
        if (source.empty())
            source = files::Utf8(bundle.stem());
        return Observation(haveReport ? &report : nullptr, source, settings.value("project", std::string()), summary.value("version", std::string()), "failed",
                           FailedStep(console), summary.value("error", std::string()));
    }
} // namespace w2b::pipeline::knowledge
