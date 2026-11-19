#pragma once
// Port of sounds.write_t6_bank with the audioremaster mix it applies: the
// official BO2 alias CSV of every PCM- and curve-bound WaW alias.

#include "core/pytext.h"
#include "sound/remaster_tables.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::sound
{
    // the loaded-bank budget: what the largest stock BO2 map bank holds, and how the map's loaded sounds fit it
    struct SoundBudget
    {
        std::string referenceBank;
        long long referenceEntries = 0, referenceBytes = 0, templateEntries = 0, templateBytes = 0, templateUnmatched = 0;
        long long entries = 0, stockHeadroomBytes = 0, bytes = 0;
        struct EntryPool
        {
            long long size = 0, alwaysLoaded = 0, templateEntries = 0;
            std::optional<long long> map; // the map bank's files, once fitted
        } entryPool;
        std::optional<std::vector<std::string>> played; // the sounds the map plays, until fitted
        struct Fit
        {
            long long loadedFiles = 0, loadedBytes = 0, streamedFiles = 0;
            long long weaponFilesLoaded = 0, weaponFilesStreamed = 0, playedFilesLoaded = 0, playedFilesStreamed = 0;
        };
        std::optional<Fit> fit;
        py::Json ToJson() const;
    };

    struct BankRequest
    {
        std::filesystem::path project;     // the project's content_source
        std::string bank;                  // waw_<project>.all
        std::filesystem::path destination; // the project's soundbank folder
        std::optional<SoundBudget> budget; // none without BO2
        py::Json reverbSend;               // the measured stock reverb send, null when unmeasured
        std::optional<RemasterTables> remaster; // the stock BO2 mix, none without BO2
        std::vector<std::string> engineNames;
        std::string flacEncoder;
        std::optional<bool> streamFlac;    // default: StreamFlacEnabled()
    };

    struct BankResult
    {
        py::Json report; // content_source/sounds.bank.json
        std::optional<SoundBudget> budget; // the budget as fit
    };

    BankResult WriteT6Bank(const BankRequest& request);
    // sounds.default_reverb_mix(content_source)
    py::Json DefaultReverbMixOf(const std::filesystem::path& project);
    // animnotes.notification_offset: where a raw v17 xanim's note count sits
    size_t NotificationOffsetOf(const std::string& raw);
} // namespace w2b::sound
