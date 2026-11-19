#pragma once
// Port of sounds.write_t6_bank with the audioremaster mix it applies: the
// official BO2 alias CSV of every PCM- and curve-bound WaW alias.

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::sound
{
    struct BankRequest
    {
        std::filesystem::path project;     // the project's content_source
        std::string bank;                  // waw_<project>.all
        std::filesystem::path destination; // the project's soundbank folder
        py::Json budget;                   // the loaded-bank budget, null without BO2
        py::Json reverbSend;               // the measured stock reverb send, null when unmeasured
        py::Json remaster;                 // the stock BO2 mix profiles, null without BO2
        std::vector<std::string> engineNames;
        std::string flacEncoder;
        std::optional<bool> streamFlac;    // default: StreamFlacEnabled()
    };

    struct BankResult
    {
        py::Json report; // content_source/sounds.bank.json
        py::Json budget; // the budget as fit
    };

    BankResult WriteT6Bank(const BankRequest& request);
    // sounds.default_reverb_mix(content_source)
    py::Json DefaultReverbMixOf(const std::filesystem::path& project);
    // animnotes.notification_offset: where a raw v17 xanim's note count sits
    size_t NotificationOffsetOf(const std::string& raw);
} // namespace w2b::sound
