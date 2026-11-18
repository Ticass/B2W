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
    Result RunMerged(const std::vector<std::string>& args, const std::string& cwd, void (*onLine)(const std::string&))
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE outRead{}, outWrite{};
        PROCESS_INFORMATION info{};
        auto command = Wide(CommandLine(args));
        const auto folder = Wide(cwd);
        BOOL created;
        {
            static std::mutex launch;
            std::lock_guard lock(launch);
            if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0))
                throw std::runtime_error("could not create child process pipes");
            HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = nul;
            startup.hStdOutput = outWrite;
            startup.hStdError = outWrite;
            created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     folder.empty() ? nullptr : folder.c_str(), &startup, &info);
            CloseHandle(outWrite);
            CloseHandle(nul);
        }
        if (!created)
        {
            CloseHandle(outRead);
            throw std::runtime_error("cannot start " + (args.empty() ? std::string() : args[0]));
        }
        Result result;
        char buffer[65536];
        DWORD read = 0;
        size_t lineStart = 0;
        while (ReadFile(outRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            result.out.append(buffer, read);
            if (onLine)
                for (size_t i = lineStart; i < result.out.size(); i++)
                    if (result.out[i] == '\n')
                    {
                        auto line = result.out.substr(lineStart, i - lineStart);
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                            line.pop_back();
                        onLine(line);
                        lineStart = i + 1;
                    }
        }
        if (onLine && lineStart < result.out.size())
            onLine(result.out.substr(lineStart));
        CloseHandle(outRead);
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
        result.exitCode = static_cast<int>(code);
        return result;
    }
    int RunInherited(const std::vector<std::string>& args, const std::vector<std::wstring>& env, void (*onExit)(void*, void*), void* context)
    {
        std::wstring block;
        if (!env.empty())
        {
            std::vector<std::wstring> entries;
            if (wchar_t* current = GetEnvironmentStringsW())
            {
                for (const wchar_t* p = current; *p; p += wcslen(p) + 1)
                    entries.emplace_back(p);
                FreeEnvironmentStringsW(current);
            }
            for (const auto& extra : env)
            {
                const auto name = extra.substr(0, extra.find(L'=') + 1);
                std::erase_if(entries, [&](const std::wstring& e) { return _wcsnicmp(e.c_str(), name.c_str(), name.size()) == 0; });
                entries.push_back(extra);
            }
            for (const auto& e : entries)
                block += e + L'\0';
            block += L'\0';
        }
        auto command = Wide(CommandLine(args));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
                            env.empty() ? nullptr : block.data(), nullptr, &startup, &info))
            throw std::runtime_error("cannot start " + (args.empty() ? std::string() : args[0]));
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        if (onExit)
            onExit(info.hProcess, context);
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
        return static_cast<int>(code);
    }
} // namespace w2b::process
