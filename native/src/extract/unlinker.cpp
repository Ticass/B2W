// Unlinker runs (see unlinker.h).
#include "extract/unlinker.h"

#include "core/files.h"
#include "extract/cache.h"
#include "oat/run_unlinker.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace w2b::extract
{
    namespace
    {
        std::string Lowered(const fs::path& path)
        {
            auto text = files::Utf8(Resolve(path).make_preferred());
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    } // namespace

    bool IsLinkedUnlinker(const fs::path& tool)
    {
        static const auto bin = ResourceRoot() / "vendor/OpenAssetTools/build/bin";
        const auto path = Lowered(tool);
        return std::ranges::any_of(std::array{"Release_x64", "Release_x86"},
                                   [&](const char* folder) { return path == Lowered(bin / folder / "Unlinker.exe"); });
    }

    std::string UnlinkerIdentity(const fs::path& tool)
    {
        return IsLinkedUnlinker(tool) ? std::string(oat::Version()) : Digest(tool);
    }

    process::Result RunUnlinker(const std::vector<std::string>& command, void (*onLine)(const std::string&))
    {
        if (IsLinkedUnlinker(files::FromUtf8(command.front())))
            if (auto run = oat::TryRunUnlinker({command.begin() + 1, command.end()}))
            {
                if (onLine)
                    for (const auto line : util::SplitLines(run->log))
                        onLine(std::string(line));
                process::Result result;
                result.exitCode = run->exitCode;
                result.out = std::move(run->log);
                return result;
            }
        return process::RunMerged(command, "", onLine);
    }
} // namespace w2b::extract
