// Knowledge ledger (see knowledge.h).
#include "knowledge.h"

#include "core/files.h"
#include "core/pyre.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <ctime>
#include <map>
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
        obs["schema"] = 1;
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
} // namespace w2b::pipeline::knowledge
