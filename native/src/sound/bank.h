#pragma once
// Port of sounds.write_t6_bank with the audioremaster mix it applies: the
// official BO2 alias CSV of every PCM- and curve-bound WaW alias.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::sound
{
    // request {project, bank, destination, budget, reverb_send, remaster, engine_names};
    // returns {"report": sounds.bank.json, "budget": the budget as fit}
    py::Json WriteT6Bank(const std::filesystem::path& request);
    // sounds.default_reverb_mix(content_source)
    py::Json DefaultReverbMixOf(const std::filesystem::path& project);
} // namespace w2b::sound
