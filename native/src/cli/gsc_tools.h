#pragma once
// Script diagnostics of waw2bo2n; nothing here takes part in a conversion.

#include <string>
#include <vector>

namespace w2b::cli
{
    using Arguments = std::vector<std::string>;

    // gsc-check <folder or script> [--bo2] [--dump <file>]: parses every .gsc/.csc as WaW compiles it
    // (--bo2: as the converter writes it), and counts what the translator's VM rules change
    int GscCheck(const Arguments& args);
    // gsc-rules <in.gsc> <out.gsc> <runtime script>: a script with T4's VM rules applied
    int GscRules(const Arguments& args);
    // gsc-link <folder of translated scripts> [<path prefix>...]: what T6 would not resolve
    int GscLink(const Arguments& args);
    // gsc-port <corpus of tools/build_gsc_corpus.ps1> <stock WaW scripts> <BO2 folder> <output> [<map>...]: translates
    // every map's scripts and link-checks the result, without building anything
    int GscPort(const Arguments& args);
} // namespace w2b::cli
