// A scripted stand-in for the OAT unlinker in extraction tests: writes a
// material and an image into --output-folder (only the kinds --include-assets
// names, when given), or lists them with --list. It counts its runs in
// <zone>.runs and records each --include-assets value in <zone>.kinds.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int wmain(const int argc, wchar_t** argv)
{
    namespace fs = std::filesystem;
    fs::path output, zone;
    std::wstring kinds;
    bool list = false;
    for (int i = 1; i < argc; i++)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--output-folder" && i + 1 < argc)
            output = argv[++i];
        else if (arg == L"--include-assets" && i + 1 < argc)
            kinds = argv[++i];
        else if (arg == L"--search-path" || arg == L"--image-format" || arg == L"--model-format")
            i++;
        else if (arg == L"--list")
            list = true;
        else if (!arg.starts_with(L"--"))
            zone = arg;
    }
    {
        const auto counter = fs::path(zone.wstring() + L".runs");
        int runs = 0;
        std::ifstream(counter) >> runs;
        std::ofstream(counter) << runs + 1;
        if (!kinds.empty())
            std::wofstream(fs::path(zone.wstring() + L".kinds"), std::ios::app) << kinds << L"\n";
    }
    if (list)
    {
        std::printf("material,mc/foo\nimage,bar\nimage,,referenced\nnot a listing line\n");
        return 0;
    }
    const auto wanted = [&](const wchar_t* kind) { return kinds.empty() || (L"," + kinds + L",").find(L"," + std::wstring(kind) + L",") != std::wstring::npos; };
    if (wanted(L"material"))
    {
        fs::create_directories(output / "materials" / "mc");
        std::ofstream(output / "materials" / "mc" / "foo.json") << "{\"techniqueSet\": \"\"}";
    }
    if (wanted(L"image"))
    {
        fs::create_directories(output / "images");
        std::ofstream(output / "images" / "bar.iwi") << "IWi";
    }
    std::printf("Dumped 2 assets\n");
    return 0;
}
