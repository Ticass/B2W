#pragma once
// Port of timeline.py: the machine-readable build timeline. When
// WAW2BO2_TIMELINE names a file, timed spans and child processes append one
// JSON line each to this process's part (<file>.parts/<pid>.jsonl); merge()
// joins the parts. Records name their parent, so nesting is explicit. Timing
// never changes a build: failures to record are ignored.

#include "pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::timeline
{
    std::optional<std::filesystem::path> Path();
    std::string NewId();
    // this thread's innermost open span, else the inherited parent
    std::optional<std::string> Current();
    void Emit(const py::Json& record);

    // a timed block; Counts() takes extra fields recorded with it
    class Span
    {
    public:
        explicit Span(const std::string& name, py::Json fields = py::Json::object());
        ~Span();
        Span(const Span&) = delete;
        Span& operator=(const Span&) = delete;
        py::Json& Counts()
        {
            return extra;
        }
        void Fail(const std::string& error);
        const std::string& Id() const
        {
            return record["id"].get_ref<const std::string&>();
        }

    private:
        py::Json record, extra = py::Json::object();
        bool active = false, failed = false;
        double wallStart = 0, cpuStart = 0;
    };

    // nest this thread's records under another thread's span
    class Parent
    {
    public:
        explicit Parent(const std::optional<std::string>& id);
        ~Parent();

    private:
        bool pushed = false;
    };

    // a child process's record: start before launching it (the child inherits
    // WAW2BO2_TIMELINE_PARENT = Id()), finish with its handle when it exits
    struct ProcessRecord
    {
        explicit ProcessRecord(const std::vector<std::string>& command);
        std::wstring ParentVariable() const; // L"WAW2BO2_TIMELINE_PARENT=<id>" or empty
        void Finish(void* processHandle, int returncode);
        py::Json record;
        double clock = 0;
        bool active = false;
    };

    py::Json SelfAccounting();
    void Start(const std::filesystem::path& target);
    std::vector<py::Json> Load(const std::filesystem::path& target);
    void Merge(const std::filesystem::path& target);
    py::Json Summarize(const std::vector<py::Json>& records);
} // namespace w2b::timeline
