#include "materials/prepare.h"

#include "core/files.h"
#include "core/parallel.h"
#include "materials/material.h"
#include "materials/techsets.h"
#include "materials/waw_material_json.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace w2b::materials
{
    using py::Json;
    using files::fs::path;

    Donors::Donors(const Json& paths)
            {
                for (const auto& [name, source] : paths.items())
                {
                    candidates.push_back(ParseTechnique(name));
                    paths_.emplace(name, files::FromUtf8(source.get<std::string>()));
                }
            }

    const Json& Donors::Get(const std::string& name)
            {
                // The stock donor files are shared by many converted materials.
                std::lock_guard guard(lock_);
                auto it = cached_.find(name);
                if (it == cached_.end())
                    it = cached_.emplace(name, Json::parse(files::ReadText(paths_.at(name)))).first;
                return it->second;
            }

    Json SelectMaterial(const WawMaterial& input, bool programs, bool layered, Donors& donors)
        {
            const auto& original = input.techniqueSet;
            auto source = SourceTechnique(input);
            const auto alternatives = LastResort(source);
            auto alternative = alternatives.begin();
            bool approximated = false;
            std::string firstError;
            std::vector<std::string> attempts;
            std::set<std::string> rejected;
            for (;;)
            {
                Match match;
                try
                {
                    match = MatchTechnique(source, donors.candidates, rejected, programs, layered);
                }
                catch (const TechniqueError& error)
                {
                    if (firstError.empty())
                        firstError = error.what();
                    if (alternative != alternatives.end())
                    {
                        source = *alternative++;
                        approximated = true;
                        rejected.clear();
                        continue;
                    }
                    if (!attempts.empty())
                    {
                        firstError += "; rejected: ";
                        for (std::size_t i = 0; i < attempts.size(); ++i)
                            firstError += (i ? "; " : "") + attempts[i];
                    }
                    return {{"error", firstError}};
                }

                auto notes = match.notes;
                if (approximated)
                    notes.push_back("APPROXIMATED: no T6 rule for '" + original + "', drawn as '" + source + "'");
                try
                {
                    auto material = BuildMaterial(input, donors.Get(match.target), notes);
                    for (const auto& attempt : attempts)
                        notes.push_back(attempt + ", not used");
                    return {{"target", match.target}, {"cost", match.cost}, {"match_notes", std::move(match.notes)},
                            {"notes", std::move(notes)}, {"material", std::move(material)},
                            {"approximation", approximated ? Json(source) : Json{}}};
                }
                catch (const TechniqueError& error)
                {
                    attempts.push_back(match.target + ": " + error.what());
                    rejected.insert(std::move(match.target));
                }
            }
        }
}
