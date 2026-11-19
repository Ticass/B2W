#pragma once
// Port of sounds.write_t6_bank with the audioremaster mix it applies: the
// official BO2 alias CSV of every PCM- and curve-bound WaW alias.

#include "core/pytext.h"

#include <filesystem>
#include <string>

namespace w2b::sound
{
    // request {project, bank, destination, budget, reverb_send, remaster, engine_names};
    // returns {"report": sounds.bank.json, "budget": the budget as fit}
    py::Json WriteT6Bank(const py::Json& request);
    // sounds.default_reverb_mix(content_source)
    py::Json DefaultReverbMixOf(const std::filesystem::path& project);
    // animnotes.notification_offset: where a raw v17 xanim's note count sits
    size_t NotificationOffsetOf(const std::string& raw);
} // namespace w2b::sound
