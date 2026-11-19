#pragma once
// What a bridge step adds to the bridge report: its warnings and errors, and
// the content sections it owns (bridge_stage.report.json "content").

#include "core/pytext.h"

#include <string>
#include <utility>
#include <vector>

namespace w2b::pipeline
{
    struct StepReport
    {
        std::vector<std::string> warnings, errors;
        std::vector<std::pair<std::string, py::Json>> content; // section -> its report value, in step order

        void Warn(std::string text)
        {
            warnings.push_back(std::move(text));
        }
        void Fail(std::string text)
        {
            errors.push_back(std::move(text));
        }
        void Add(std::string section, py::Json value)
        {
            content.emplace_back(std::move(section), std::move(value));
        }
        // appends this step's lists to report["warnings"/"errors"] and sets its content sections
        void MergeInto(py::Json& report) const
        {
            for (const auto& warning : warnings)
                report["warnings"].push_back(warning);
            for (const auto& error : errors)
                report["errors"].push_back(error);
            for (const auto& [section, value] : content)
                report["content"][section] = value;
        }
    };
} // namespace w2b::pipeline
