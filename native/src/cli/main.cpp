// waw2bo2n: the converter's native steps, one subcommand each.
#include "core/files.h"
#include "shaders/stage.h"

#include <windows.h>

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace
{
    std::string Utf8(const wchar_t* text)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
        return out;
    }

    int Usage()
    {
        std::fputs("usage: waw2bo2n shaders-stage --dest <folder> <root>...\n", stderr);
        return 2;
    }

    int ShadersStage(const std::vector<std::string>& args)
    {
        std::string dest;
        std::vector<std::string> roots;
        for (size_t i = 0; i < args.size(); i++)
        {
            if (args[i] == "--dest" && i + 1 < args.size())
                dest = args[++i];
            else
                roots.push_back(args[i]);
        }
        if (dest.empty())
            return Usage();
        const auto report = w2b::shaders::Stage(roots, w2b::files::FromUtf8(dest));
        std::printf("shaders: %lld translated, %lld unsupported\n", report["translated"].get<long long>(), report["unsupported"].get<long long>());
        return 0;
    }
} // namespace

int wmain(const int argc, wchar_t** argv)
{
    if (argc < 2)
        return Usage();
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++)
        args.push_back(Utf8(argv[i]));
    const std::string command = Utf8(argv[1]);
    try
    {
        if (command == "shaders-stage")
            return ShadersStage(args);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "waw2bo2n %s: %s\n", command.c_str(), error.what());
        return 1;
    }
    return Usage();
}
