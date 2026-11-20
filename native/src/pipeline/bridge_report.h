#pragma once
// The bridge stage's report (zone_raw/<project>/bridge_stage.report.json):
// every staged material and image, the technique sets, the map's collision,
// entities, scripts and content summaries, its effect table, and the
// warnings and errors of every pass.

#include "core/text.h"
#include "pipeline/material_record.h"

#include <map>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct BridgeReport
    {
        std::string project;
        std::vector<MaterialRecord> materials;
        util::Json images = util::Json::array();      // staged image records
        std::vector<std::string> techsets;
        util::Json collision = util::Json::object();
        util::Json entities = util::Json::object();
        util::Json scripts = util::Json::object();    // the ported scripts' report, empty without scripts
        util::Json content = util::Json::object();    // per-pass content sections, in pass order
        std::map<std::string, std::string> fxTable; // script-loaded WaW effect -> converted name
        std::vector<std::string> errors, warnings;

        void Warn(std::string text)
        {
            warnings.push_back(std::move(text));
        }
        void Fail(std::string text)
        {
            errors.push_back(std::move(text));
        }
        // the scripts section's value for `key`, `fallback` when the map has no scripts or no such entry
        util::Json Script(const char* key, util::Json fallback = util::Json::array()) const
        {
            return scripts.is_object() && scripts.contains(key) ? scripts[key] : fallback;
        }
        util::Json ToJson() const;
    };
} // namespace w2b::pipeline
