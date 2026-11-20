// Build timeline (see timeline.h).
#include "timeline.h"

#include "files.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

#pragma comment(lib, "psapi.lib")

namespace w2b::timeline
{
    namespace fs = std::filesystem;

    namespace
    {
        std::atomic<long long> ids{1};
        std::mutex writeMutex;
        thread_local std::vector<std::string> stack;

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        double Clock()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        double Round(const double value, const int digits)
        {
            const double scale = std::pow(10.0, digits);
            return std::nearbyint(value * scale) / scale;
        }

        double ProcessCpu(HANDLE process)
        {
            FILETIME created, exited, kernel, user;
            if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
                return 0;
            const auto ticks = [](const FILETIME& t) { return static_cast<double>(static_cast<unsigned long long>(t.dwHighDateTime) << 32 | t.dwLowDateTime); };
            return (ticks(kernel) + ticks(user)) / 1e7;
        }

        std::wstring Env(const wchar_t* name)
        {
            const wchar_t* value = _wgetenv(name);
            return value ? value : L"";
        }

        util::Json Accounting(HANDLE process)
        {
            util::Json out = util::Json::object();
            if (!process)
                return out;
            out["cpu"] = Round(ProcessCpu(process), 4);
            IO_COUNTERS io;
            if (GetProcessIoCounters(process, &io))
            {
                out["read_mb"] = Round(static_cast<double>(io.ReadTransferCount) / 1048576.0, 2);
                out["write_mb"] = Round(static_cast<double>(io.WriteTransferCount) / 1048576.0, 2);
            }
            PROCESS_MEMORY_COUNTERS memory{sizeof memory};
            if (GetProcessMemoryInfo(process, &memory, sizeof memory))
                out["peak_mb"] = Round(static_cast<double>(memory.PeakWorkingSetSize) / 1048576.0, 1);
            return out;
        }

        std::string Describe(const std::vector<std::string>& command)
        {
            std::vector<std::string> parts = command;
            if (!parts.empty())
                parts[0] = files::Utf8(files::FromUtf8(parts[0]).filename());
            const auto m = std::find(parts.begin(), parts.end(), "-m");
            if (m != parts.end() && m + 2 < parts.end())
                parts = {*(m + 1), *(m + 2)};
            std::string text;
            for (size_t i = 0; i < parts.size(); i++)
                text += (i ? " " : "") + parts[i];
            return text.substr(0, 160);
        }

        fs::path Parts(const fs::path& target)
        {
            return target.parent_path() / (target.filename().wstring() + L".parts");
        }
    } // namespace

    std::optional<fs::path> Path()
    {
        const auto value = Env(L"WAW2BO2_TIMELINE");
        if (value.empty())
            return std::nullopt;
        return fs::path(value);
    }

    std::string NewId()
    {
        return std::to_string(GetCurrentProcessId()) + "." + std::to_string(ids++);
    }

    std::optional<std::string> Current()
    {
        if (!stack.empty())
            return stack.back();
        const auto parent = Env(L"WAW2BO2_TIMELINE_PARENT");
        if (parent.empty())
            return std::nullopt;
        return files::Utf8(fs::path(parent));
    }

    void Emit(const util::Json& record)
    {
        const auto target = Path();
        if (!target)
            return;
        try
        {
            const auto line = util::JsonDumps(record, -1) + "\n";
            const auto part = Parts(*target) / (std::to_wstring(GetCurrentProcessId()) + L".jsonl");
            std::lock_guard lock(writeMutex);
            std::error_code e;
            fs::create_directories(part.parent_path(), e);
            std::ofstream stream(part, std::ios::binary | std::ios::app);
            stream << line;
        }
        catch (...)
        {
        }
    }

    Span::Span(const std::string& name, util::Json fields)
    {
        if (!Path())
            return;
        active = true;
        record = util::Json::object();
        record["id"] = NewId();
        const auto parent = Current();
        record["parent"] = parent ? util::Json(*parent) : util::Json();
        record["kind"] = "span";
        record["name"] = name;
        record["pid"] = GetCurrentProcessId();
        record["thread"] = "native";
        for (const auto& [k, v] : fields.items())
            record[k] = v;
        stack.push_back(record["id"].get<std::string>());
        record["start"] = Now();
        wallStart = Clock();
        cpuStart = ProcessCpu(GetCurrentProcess());
    }

    void Span::Fail(const std::string& error)
    {
        failed = true;
        if (active)
            record["error"] = error.substr(0, 300);
    }

    Span::~Span()
    {
        if (!active)
            return;
        record["ok"] = !failed && std::uncaught_exceptions() == 0;
        record["wall"] = Round(Clock() - wallStart, 4);
        record["cpu"] = Round(ProcessCpu(GetCurrentProcess()) - cpuStart, 4);
        if (!stack.empty())
            stack.pop_back();
        for (const auto& [k, v] : extra.items())
            record[k] = v;
        Emit(record);
    }

    Parent::Parent(const std::optional<std::string>& id)
    {
        if (id)
        {
            stack.push_back(*id);
            pushed = true;
        }
    }

    Parent::~Parent()
    {
        if (pushed && !stack.empty())
            stack.pop_back();
    }

    ProcessRecord::ProcessRecord(const std::vector<std::string>& command)
    {
        if (!Path())
            return;
        active = true;
        record = util::Json::object();
        record["id"] = NewId();
        const auto parent = Current();
        record["parent"] = parent ? util::Json(*parent) : util::Json();
        record["kind"] = "process";
        record["name"] = Describe(command);
        record["pid"] = nullptr;
        record["start"] = Now();
        clock = Clock();
    }

    std::wstring ProcessRecord::ParentVariable() const
    {
        if (!active)
            return L"";
        return L"WAW2BO2_TIMELINE_PARENT=" + files::FromUtf8(record["id"].get<std::string>()).wstring();
    }

    void ProcessRecord::Finish(void* process, const int returncode)
    {
        if (!active)
            return;
        active = false;
        record["pid"] = process ? GetProcessId(static_cast<HANDLE>(process)) : 0;
        record["wall"] = Round(Clock() - clock, 4);
        record["returncode"] = returncode;
        const auto accounting = Accounting(static_cast<HANDLE>(process));
        for (const auto& [k, v] : accounting.items())
            record[k] = v;
        Emit(record);
    }

    util::Json SelfAccounting()
    {
        return Accounting(GetCurrentProcess());
    }

    void Start(const fs::path& target)
    {
        std::error_code e;
        fs::create_directories(target.parent_path(), e);
        fs::remove_all(Parts(target), e);
        files::WriteBytes(target, "");
        _wputenv_s(L"WAW2BO2_TIMELINE", target.c_str());
        _wputenv_s(L"WAW2BO2_TIMELINE_PARENT", L"");
    }

    std::vector<util::Json> Load(const fs::path& target)
    {
        std::vector<util::Json> records;
        std::vector<fs::path> sources;
        std::error_code e;
        if (fs::is_regular_file(target, e))
            sources.push_back(target);
        std::vector<fs::path> parts;
        for (fs::directory_iterator it(Parts(target), e), end; !e && it != end; it.increment(e))
            if (it->path().extension() == L".jsonl")
                parts.push_back(it->path());
        std::sort(parts.begin(), parts.end());
        sources.insert(sources.end(), parts.begin(), parts.end());
        for (const auto& source : sources)
        {
            std::string text;
            files::ReadBytes(source, text);
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
                try
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (!line.empty())
                        records.push_back(util::Json::parse(line));
                }
                catch (...)
                {
                }
        }
        std::stable_sort(records.begin(), records.end(), [](const util::Json& a, const util::Json& b) { return a.value("start", 0.0) < b.value("start", 0.0); });
        return records;
    }

    void Merge(const fs::path& target)
    {
        std::string text;
        for (const auto& r : Load(target))
            text += util::JsonDumps(r, -1) + "\n";
        files::WriteBytes(target, text);
        std::error_code e;
        fs::remove_all(Parts(target), e);
    }

    util::Json Summarize(const std::vector<util::Json>& records)
    {
        if (records.empty())
            return util::Json::object();
        std::map<std::string, const util::Json*> byId;
        for (const auto& r : records)
            byId[r["id"].get<std::string>()] = &r;
        std::map<std::string, std::vector<const util::Json*>> children; // "" = roots
        for (const auto& r : records)
        {
            const auto parent = r.value("parent", util::Json());
            const std::string key = parent.is_string() && byId.count(parent.get<std::string>()) ? parent.get<std::string>() : "";
            children[key].push_back(&r);
        }
        double origin = records.front()["start"].get<double>(), end = 0;
        for (const auto& r : records)
        {
            origin = std::min(origin, r["start"].get<double>());
            end = std::max(end, r["start"].get<double>() + r.value("wall", 0.0));
        }
        const auto covered = [](const std::vector<const util::Json*>& kids)
        {
            std::vector<std::pair<double, double>> spans;
            for (const auto* k : kids)
                if ((*k)["kind"] != "worker")
                    spans.emplace_back((*k)["start"].get<double>(), (*k)["start"].get<double>() + k->value("wall", 0.0));
            std::sort(spans.begin(), spans.end());
            double total = 0, last = -1e300;
            bool any = false;
            for (const auto& [s, e] : spans)
            {
                if (!any || s > last)
                    total += e - s, last = e, any = true;
                else if (e > last)
                    total += e - last, last = e;
            }
            return total;
        };
        std::function<util::Json(const util::Json&)> tree = [&](const util::Json& r)
        {
            util::Json node = util::Json::object();
            for (const char* key : {"kind", "name", "wall", "cpu", "peak_mb", "read_mb", "write_mb", "returncode", "ok"})
                if (r.contains(key))
                    node[key] = r[key];
            node["start_offset"] = Round(r["start"].get<double>() - origin, 3);
            auto kids = children[r["id"].get<std::string>()];
            std::stable_sort(kids.begin(), kids.end(), [](const util::Json* a, const util::Json* b) { return (*a)["start"].get<double>() < (*b)["start"].get<double>(); });
            std::vector<const util::Json*> workers;
            util::Json nested = util::Json::array();
            for (const auto* k : kids)
                if ((*k)["kind"] == "worker")
                    workers.push_back(k);
                else
                    nested.push_back(tree(*k));
            if (!workers.empty())
            {
                double cpu = 0, peak = 0;
                for (const auto* w : workers)
                    cpu += w->value("cpu", 0.0), peak = std::max(peak, w->value("peak_mb", 0.0));
                node["workers"] = util::Json{{"count", workers.size()}, {"cpu", Round(cpu, 3)}, {"peak_mb_max", peak}};
            }
            if (!nested.empty())
            {
                node["children"] = nested;
                node["unattributed"] = Round(r.value("wall", 0.0) - covered(kids), 3);
            }
            return node;
        };
        auto roots = children[""];
        std::stable_sort(roots.begin(), roots.end(), [](const util::Json* a, const util::Json* b) { return (*a)["start"].get<double>() < (*b)["start"].get<double>(); });
        long long processes = 0, workerCount = 0;
        double nativeCpu = 0, workerCpu = 0, peak = 0;
        for (const auto& r : records)
        {
            if (r["kind"] == "process")
                processes++, nativeCpu += r.value("cpu", 0.0);
            if (r["kind"] == "worker")
                workerCount++, workerCpu += r.value("cpu", 0.0);
            peak = std::max(peak, r.value("peak_mb", 0.0));
        }
        util::Json out = util::Json::object();
        out["wall"] = Round(end - origin, 3);
        out["process_count"] = processes;
        out["native_cpu"] = Round(nativeCpu, 3);
        out["worker_count"] = workerCount;
        out["worker_cpu"] = Round(workerCpu, 3);
        out["peak_mb_max"] = peak;
        util::Json treeList = util::Json::array();
        for (const auto* r : roots)
            treeList.push_back(tree(*r));
        out["tree"] = treeList;
        return out;
    }
} // namespace w2b::timeline
