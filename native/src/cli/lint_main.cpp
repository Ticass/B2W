#include "core/encoding.h"
#include "core/files.h"
#include "core/parallel.h"
#include "core/process.h"
#include "core/pytext.h"

#include <windows.h>

#include <array>
#include <charconv>
#include <iostream>
#include <set>

namespace
{
    namespace fs = std::filesystem;
    using w2b::files::FromUtf8;
    using w2b::files::Utf8;

    struct Options
    {
        fs::path build;
        fs::path clangTidy = L"clang-tidy";
        unsigned jobs = std::min(8u, w2b::parallel::Workers());
    };

    void Usage(std::ostream& stream)
    {
        stream << "Usage: w2b_lint <CMake build folder> [--jobs N] [--clang-tidy <path>]\n";
    }

    Options Parse(int count, wchar_t** arguments)
    {
        Options options;
        for (int index = 1; index < count; ++index)
        {
            const std::wstring_view argument = arguments[index];
            if (argument == L"--jobs" && index + 1 < count)
            {
                const auto text = Utf8(fs::path(arguments[++index]));
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), options.jobs);
                if (error != std::errc{} || end != text.data() + text.size() || options.jobs == 0)
                    throw std::invalid_argument("--jobs must be a positive integer");
            }
            else if (argument == L"--clang-tidy" && index + 1 < count)
                options.clangTidy = arguments[++index];
            else if (!argument.starts_with(L"-") && options.build.empty())
                options.build = argument;
            else
                throw std::invalid_argument("unexpected argument " + Utf8(fs::path(argument)));
        }
        if (options.build.empty())
            throw std::invalid_argument("specify the CMake build folder");
        return options;
    }

    fs::path FindLinter(const fs::path& tool)
    {
        std::array<wchar_t, 32768> path{};
        const auto count = SearchPathW(nullptr, tool.c_str(), L".exe", static_cast<DWORD>(path.size()), path.data(), nullptr);
        if (count && count < path.size() && fs::is_regular_file(path.data()))
            return path.data();
        throw std::invalid_argument("clang-tidy is missing. Install it or pass its executable with --clang-tidy");
    }

    int ComparePath(const fs::path& left, const fs::path& right)
    {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE);
    }

    std::vector<fs::path> CompilationFiles(const fs::path& database, const fs::path& native)
    {
        const auto entries = w2b::py::Json::parse(w2b::files::ReadText(database));
        const auto order = [](const fs::path& left, const fs::path& right) { return ComparePath(left, right) == CSTR_LESS_THAN; };
        std::set<fs::path, decltype(order)> files;
        for (const auto& entry : entries)
        {
            const auto path = fs::weakly_canonical(FromUtf8(entry.at("directory").get<std::string>()) / FromUtf8(entry.at("file").get<std::string>()));
            if (path.extension() != ".cpp")
                continue;
            for (const auto* folder : {"src", "tests"})
            {
                const auto scope = native / folder;
                if (std::ranges::mismatch(scope, path, [](const auto& left, const auto& right)
                    { return ComparePath(left, right) == CSTR_EQUAL; }).in1 == scope.end())
                    files.insert(path);
            }
        }
        return {files.begin(), files.end()};
    }

    int Lint(const Options& options)
    {
        const auto tool = Utf8(FindLinter(options.clangTidy));
        const auto build = fs::weakly_canonical(fs::absolute(options.build));
        const auto database = build / "compile_commands.json";
        if (!fs::is_regular_file(database))
            throw std::invalid_argument(Utf8(database) + " is missing. Configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
        const auto native = fs::weakly_canonical(FromUtf8(W2B_NATIVE_SOURCE_DIR));
        const auto files = CompilationFiles(database, native);
        if (files.empty())
            throw std::invalid_argument("the compilation database has no converter C++ files");

        const auto buildPath = Utf8(build);
        std::vector<w2b::process::Result> results(files.size());
        w2b::parallel::For(files.size(), [&](std::size_t index)
        {
            results[index] = w2b::process::Run({tool, "-p", buildPath, Utf8(files[index])}, {});
        }, options.jobs);

        std::size_t failures = 0;
        for (std::size_t index = 0; index < files.size(); ++index)
        {
            const auto& result = results[index];
            if (result.exitCode || !w2b::py::Strip(result.out).empty())
                std::cout << Utf8(files[index].lexically_relative(native)) << '\n'
                          << w2b::py::RStrip(w2b::encoding::Utf8Replace(result.out + result.err), " \t\r\n") << '\n';
            failures += result.exitCode != 0;
        }
        std::cout << "clang-tidy checked " << files.size() << " C++ files, " << failures << " failed.\n";
        return failures ? 1 : 0;
    }
}

int wmain(int count, wchar_t** arguments)
{
    if (count == 2 && (std::wstring_view(arguments[1]) == L"--help" || std::wstring_view(arguments[1]) == L"-h"))
    {
        Usage(std::cout);
        return 0;
    }
    try
    {
        return Lint(Parse(count, arguments));
    }
    catch (const std::invalid_argument& error)
    {
        Usage(std::cerr);
        std::cerr << "w2b_lint: " << error.what() << '\n';
        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "w2b_lint: " << error.what() << '\n';
        return 1;
    }
}
