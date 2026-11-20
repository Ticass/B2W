#pragma once
// What a bridge step adds to the bridge report: its warnings and errors, and
// the content sections it owns (bridge_stage.report.json "content").

#include "core/text.h"

#include <string>
#include <utility>
#include <vector>

namespace w2b::pipeline
{
    struct StepReport
    {
        std::vector<std::string> warnings, errors;
        std::vector<std::pair<std::string, util::Json>> content; // section -> its report value, in step order

        void Warn(std::string text)
        {
            warnings.push_back(std::move(text));
        }
        void Fail(std::string text)
        {
            errors.push_back(std::move(text));
        }
        void Add(std::string section, util::Json value)
        {
            content.emplace_back(std::move(section), std::move(value));
        }
        // appends this step's lists to the report's warnings and errors and sets its content sections
        template <typename Report> void MergeInto(Report& report) const
        {
            report.warnings.insert(report.warnings.end(), warnings.begin(), warnings.end());
            report.errors.insert(report.errors.end(), errors.begin(), errors.end());
            for (const auto& [section, value] : content)
                report.content[section] = value;
        }
    };
} // namespace w2b::pipeline
