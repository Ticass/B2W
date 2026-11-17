#include "process.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace w2b::process
{
    namespace
    {
        std::wstring Wide(const std::string& text)
        {
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
            return out;
        }

        void Drain(HANDLE pipe, std::string& into)
        {
            char buffer[65536];
            DWORD read = 0;
            while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                into.append(buffer, read);
        }
    } // namespace

    std::string CommandLine(const std::vector<std::string>& args)
    {
        std::string result;
        for (const auto& arg : args)
        {
            if (!result.empty())
                result += ' ';
            const bool quote = arg.empty() || arg.find_first_of(" \t") != std::string::npos;
            if (quote)
                result += '"';
            size_t backslashes = 0;
            for (const char c : arg)
            {
                if (c == '\\')
                {
                    backslashes++;
                    continue;
                }
                if (c == '"')
                {
                    result += std::string(backslashes * 2, '\\');
                    result += "\\\"";
                }
                else
                {
                    result += std::string(backslashes, '\\');
                    result += c;
                }
                backslashes = 0;
            }
            result += std::string(quote ? backslashes * 2 : backslashes, '\\');
            if (quote)
                result += '"';
        }
        return result;
    }

    Result Run(const std::vector<std::string>& args, const std::string_view input, const double timeoutSeconds)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE inRead{}, inWrite{}, outRead{}, outWrite{}, errRead{}, errWrite{};

        PROCESS_INFORMATION info{};
        auto command = Wide(CommandLine(args));
        BOOL created;
        {
            // Restrict the lifetime of inheritable pipes to one launch at a
            // time. A sibling inheriting a writer would delay the reader's EOF.
            static std::mutex launch;
            std::lock_guard lock(launch);
            if (!CreatePipe(&inRead, &inWrite, &inherit, 0) ||
                !CreatePipe(&outRead, &outWrite, &inherit, 0) ||
                !CreatePipe(&errRead, &errWrite, &inherit, 0) ||
                !SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0) ||
                !SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0) ||
                !SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0))
            {
                for (const auto pipe : {inRead, inWrite, outRead, outWrite, errRead, errWrite})
                    if (pipe)
                        CloseHandle(pipe);
                throw std::runtime_error("could not create child process pipes");
            }
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = inRead;
            startup.hStdOutput = outWrite;
            startup.hStdError = errWrite;
            created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &startup, &info);
            CloseHandle(inRead);
            CloseHandle(outWrite);
            CloseHandle(errWrite);
        }
        Result result;
        if (!created)
        {
            CloseHandle(inWrite);
            CloseHandle(outRead);
            CloseHandle(errRead);
            throw std::runtime_error("cannot start " + (args.empty() ? std::string() : args[0]));
        }
        std::thread writer(
            [&]
            {
                size_t done = 0;
                while (done < input.size())
                {
                    DWORD wrote = 0;
                    const auto chunk = static_cast<DWORD>(std::min<size_t>(input.size() - done, 1 << 20));
                    if (!WriteFile(inWrite, input.data() + done, chunk, &wrote, nullptr))
                        break;
                    done += wrote;
                }
                CloseHandle(inWrite);
            });
        std::thread outReader([&] { Drain(outRead, result.out); });
        std::thread errReader([&] { Drain(errRead, result.err); });
        const DWORD wait = WaitForSingleObject(info.hProcess, static_cast<DWORD>(timeoutSeconds * 1000));
        if (wait == WAIT_TIMEOUT)
        {
            result.timedOut = true;
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, INFINITE);
        }
        writer.join();
        outReader.join();
        errReader.join();
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        result.exitCode = static_cast<int>(code);
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
        CloseHandle(outRead);
        CloseHandle(errRead);
        return result;
    }
} // namespace w2b::process
