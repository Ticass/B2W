#pragma once
// The knowledge ledger: one observation per converted map (stage report gaps,
// reported fallbacks, the step that failed), and the converter's backlog
// ranked across every map it has seen. Evidence only: no conversion decision
// reads it.

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace w2b::pipeline::knowledge
{
    inline constexpr int SCHEMA = 1;
    inline constexpr const char* REPORT_NAME = "bridge_stage.report.json";
    std::pair<std::string, std::string> Classify(const std::string& entry);
    std::string ErrorKind(const std::string& text);
    std::string MapKey(const std::string& source);
    util::Json Observation(const util::Json* report, const std::string& source, const std::string& project, const std::string& version, const std::string& outcome,
                         const std::optional<std::string>& step, const std::string& error);
    std::filesystem::path Record(const std::filesystem::path& ledger, const util::Json& observation);

    // the last "== N. step" banner of a build console
    std::optional<std::string> FailedStep(const std::string& log);
    // every map's observation in the ledgers, a map's newest record winning
    std::vector<util::Json> Load(const std::vector<std::filesystem::path>& ledgers);
    // gaps ranked by the number of maps sharing them, failed steps, stage errors
    util::Json Aggregate(const std::vector<util::Json>& observations);
    // the observation a failure bundle (diagnostics) carries
    std::optional<util::Json> FromBundle(const std::filesystem::path& bundle);
} // namespace w2b::pipeline::knowledge
