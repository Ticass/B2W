#pragma once
// Developer diagnostics of waw2bo2n: tools that read a build's or the game's
// files and print what they find; nothing here takes part in a conversion.

#include <string>
#include <vector>

namespace w2b::cli
{
    using Arguments = std::vector<std::string>;

    // knowledge-report [--ledger <folder>]... [--bundle <zip>]... [--report <bridge_stage.report.json>]...
    //                  [--record] [--write <summary.json>] [--top <n>]
    int KnowledgeReport(const Arguments& args);
    // ipak-check <file.ipak>: entries t6zm's image streamer would read past a window
    int IpakCheck(const Arguments& args);
    // crash-summary <dump.dmp>...: the faulting module + offset, registers, stack returns
    int CrashSummary(const Arguments& args);
} // namespace w2b::cli
