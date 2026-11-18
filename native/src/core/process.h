#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace w2b::process
{
    struct Result
    {
        int exitCode = -1;
        bool timedOut = false;
        std::string out;
        std::string err;
    };

    // subprocess.run(args, input=input, capture_output=True, timeout=seconds):
    // arguments quoted as subprocess.list2cmdline quotes them.
    // A negative timeout waits for the process to finish.
    Result Run(const std::vector<std::string>& args, std::string_view input, double timeoutSeconds = -1.0);

    // subprocess.run(args, cwd=cwd, stdout=PIPE, stderr=STDOUT): one merged output;
    // onLine (optional) sees each line as it arrives (verbose tool streaming)
    Result RunMerged(const std::vector<std::string>& args, const std::string& cwd = "", void (*onLine)(const std::string&) = nullptr);

    // subprocess.run(args) with this console: the child writes straight to our
    // stdout/stderr. env: extra NAME=value entries; handle receives the
    // process handle before it is closed (timeline accounting), when given.
    int RunInherited(const std::vector<std::string>& args, const std::vector<std::wstring>& env = {}, void (*onExit)(void* handle, void* context) = nullptr,
                     void* context = nullptr);

    // subprocess.list2cmdline
    std::string CommandLine(const std::vector<std::string>& args);
} // namespace w2b::process
