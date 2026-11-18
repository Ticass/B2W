#pragma once
// Port of knowledge.py's ledger writer: one observation per converted map
// (stage report gaps, reported fallbacks, the step that failed). Evidence only:
// no conversion decision reads it.

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace w2b::pipeline::knowledge
{
    inline constexpr const char* REPORT_NAME = "bridge_stage.report.json";
    std::pair<std::string, std::string> Classify(const std::string& entry);
    std::string ErrorKind(const std::string& text);
    std::string MapKey(const std::string& source);
    py::Json Observation(const py::Json* report, const std::string& source, const std::string& project, const std::string& version, const std::string& outcome,
                         const std::optional<std::string>& step, const std::string& error);
    std::filesystem::path Record(const std::filesystem::path& ledger, const py::Json& observation);
} // namespace w2b::pipeline::knowledge
