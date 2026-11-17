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
    Result Run(const std::vector<std::string>& args, std::string_view input, double timeoutSeconds);

    // subprocess.list2cmdline
    std::string CommandLine(const std::vector<std::string>& args);
} // namespace w2b::process
