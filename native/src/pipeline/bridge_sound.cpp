#include "pipeline/bridge_sound.h"

#include "audio/stage.h"
#include "extract/stockwaw.h"
#include "pipeline/amb_stage.h"
#include "pipeline/bridge_support.h"
#include "pipeline/finish.h"
#include "pipeline/sound_mix.h"
#include "pipeline/sound_refs.h"
#include "sound/bindings.h"
#include "sound/stage.h"

#include <algorithm>
#include <set>

namespace w2b::pipeline
{
    using bridge::AppendZone;
    using bridge::OptionalPath;
    using bridge::PathList;
    using bridge::PathOrNull;
    using bridge::Paths;
    using bridge::Timed;
    using bridge::Truthy;
    using mod::StartsWith;
    using mod::Str;
    using py::Json;

    namespace
    {
        // wawassets._zone_rank
        std::pair<int, std::string> ZoneRank(const std::string& zone)
        {
            return {StartsWith(zone, "nazi_zombie") ? 0 : StartsWith(zone, "localized") ? 2 : 1, zone};
        }

        std::string Fixed(const double v, const int digits)
        {
            char text[64];
            std::snprintf(text, sizeof text, "%.*f", digits, v);
            return text;
        }

        // repr() of a dict of str -> int (the entry pool in SOUND_ENTRY_POOL)
        std::string IntDictRepr(const Json& d)
        {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : d.items())
            {
                out += (first ? "" : ", ") + py::StringRepr(k) + ": " + (v.is_number_integer() ? std::to_string(v.get<long long>()) : py::FloatRepr(v.get<double>()));
                first = false;
            }
            return out + "}";
        }

        Json BindBridgeCurves(const fs::path& content, const TailSounds& sounds)
        {
            const auto driver = sounds.t6SoundDriver.value_or(fs::path{});
            try
            {
                return sound::BindCurves(content, driver, sounds.approximateCurves);
            }
            catch (const sound::SoundError& error)
            {
                throw StepError("SoundError", error.what());
            }
        }

    } // namespace

    void StageBridgeSound(const TailInput& input, BridgeReport& report, extract::StockWaw* stock)
    {
        const auto& sounds = input.sounds;
        const auto& project = input.project;
        const auto& projectRoot = input.projectRoot;
        const auto& roots = input.roots;
        const auto content = projectRoot / "content_source";
        auto& warnings = report.warnings;
        auto& out = report.content;
        // source_amb_script: the WaW map's clientscripts/<map>_amb.csc
        std::optional<fs::path> sourceAmb;
        if (sounds.mapStem)
            for (const auto& root : sounds.ambRoots)
            {
                const auto candidate = root / "clientscripts" / files::FromUtf8(*sounds.mapStem + "_amb.csc");
                if (mod::IsFile(candidate))
                {
                    sourceAmb = candidate;
                    break;
                }
            }
        const auto& ents = sounds.ents;
        const auto found = Timed("Resolve sound dependencies", [&] {
            return DiscoverSounds({projectRoot, roots, stock, sounds.zoneSounds, sounds.compiledSounds, ents, sourceAmb});
        });
        found.report.MergeInto(report);
        // sounds.stage over the resolver's roots (source roots, then stock zones by rank)
        std::vector<fs::path> soundRoots = roots;
        {
            auto stockRoots = found.stockRoots;
            std::sort(stockRoots.begin(), stockRoots.end(), [](const auto& a, const auto& b) { return ZoneRank(a.first) < ZoneRank(b.first); });
            for (const auto& [_, root] : stockRoots)
                soundRoots.push_back(root);
        }
        sound::StageInput stageInput;
        for (const auto& r : soundRoots)
            stageInput.roots.push_back(Str(r));
        for (const auto& d : sounds.iwdDirs)
            stageInput.iwdDirs.push_back(Str(d));
        stageInput.names = std::set<std::string>();
        stageInput.names->insert(found.soundNames.begin(), found.soundNames.end());
        // _globals_file: the map's driver globals, else the stock WaW driver's
        for (const auto& root : soundRoots)
        {
            const auto globals = files::GlobSuffix(root / "soundglobals", ".w2bsndglobals.json");
            std::vector<fs::path> direct;
            for (const auto& g : globals)
                if (g.parent_path() == root / "soundglobals")
                    direct.push_back(g);
            if (!direct.empty())
            {
                stageInput.globalsFile = Str(direct.front());
                break;
            }
        }
        if (!stageInput.globalsFile && stock)
            if (const auto hit = stock->RootFor("snddriverglobals", "singleton"))
                stageInput.globalsFile = Str(hit->second / "soundglobals/singleton.w2bsndglobals.json");
        stageInput.project = content;
        fs::create_directories(content);
        Timed("Stage sound sources", [&] { return sound::Stage(stageInput); });
        const auto soundReport = mod::Json(content / "sounds.stage.json");
        audio::Tools tools;
        tools.xwmaHelper = sounds.xwmaHelper;
        tools.resampler = sounds.resampler;
        const auto decoder = sounds.audioDecoder ? Str(*sounds.audioDecoder) : std::string();
        Timed("Decode audio", [&] { return audio::Stage(content, content / "pcm", decoder, tools); });
        const auto audioReport = mod::Json(content / "pcm" / "audio.stage.json");
        const auto bindingReport = Timed("Bind decoded audio", [&] { return sound::BindPcm(content, content / "pcm"); });
        out["animation_sound_identities"] = found.animationSoundIdentities;
        const auto curveReport = Timed("Bind sound curves", [&] { return BindBridgeCurves(content, sounds); });
        const auto bank = "waw_" + project + ".all";
        const auto mix = Timed("Build sound bank", [&] {
            return SoundMix({projectRoot, project, input.bo2Root, stock, found.scriptSounds, found.entitySounds, sourceAmb,
                             tools.resampler ? Str(*tools.resampler) : std::string()});
        });
        mix.report.MergeInto(report);
        const auto& budget = mix.budget;
        const auto& bankReport = mix.bankReport;
        WriteAmbCsc(project, projectRoot, sourceAmb, ents).MergeInto(report);
        if (budget)
        {
            const auto& fit = budget->fit.value(); // the bank always fits a budget it was given
            const auto mb = [](long long bytes) { return std::to_string(bytes / 1048576); };
            warnings.push_back("SOUND_LOADED_TO_STREAMED " + std::to_string(fit.streamedFiles) + " WaW loaded sounds streamed: loaded-bank budget " + mb(budget->bytes) +
                               " MB (largest stock map bank " + budget->referenceBank + " minus the template bank's " + mb(budget->templateBytes) + " MB); kept " +
                               std::to_string(fit.loadedFiles) + " loaded files, weapon dependencies, then played sounds (" + mb(fit.loadedBytes) + " MB); " +
                               std::to_string(fit.playedFilesStreamed) + " played files still stream (10 stream voices). List: content_source/sounds.bank.json");
            const auto& pool = budget->entryPool;
            if (pool.alwaysLoaded + pool.templateEntries + pool.map.value_or(0) > pool.size)
                warnings.push_back("SOUND_ENTRY_POOL " + IntDictRepr(budget->ToJson()["entry_pool"]) + ": T6 cannot load the map bank (entry pool full)");
        }
        if (Truthy(bankReport.at("variants")))
            AppendZone(projectRoot, "soundbank," + bank + "\n");
        out["sound_bank"] = Json{{"status", bankReport.at("status")},
                                 {"bank", bank},
                                 {"aliases", bankReport.at("aliases")},
                                 {"variants", bankReport.at("variants")},
                                 {"not_translated", bankReport.at("not_translated")},
                                 {"clamped_to_t6_range", bankReport.at("clamped").size()},
                                 {"errors", bankReport.at("errors").size()},
                                 {"report", "content_source/sounds.bank.json"}};
        files::WriteText(content / "sounds.dependencies.json", py::JsonDumps(found.soundGraph, 2) + "\n");
        out["sound_aliases"] = Json{{"status", soundReport.at("status")}, {"aliases", soundReport.at("aliases").size()}, {"audio", soundReport.at("audio").size()},
                                    {"errors", soundReport.at("errors")}, {"report", "content_source/sounds.stage.json"}};
        long long pcm = 0;
        for (const auto& a : audioReport.at("audio"))
            pcm += a.contains("output");
        out["audio"] = Json{{"status", audioReport.at("status")}, {"pcm_files", pcm}, {"errors", audioReport.at("errors")}, {"report", "content_source/pcm/audio.stage.json"}};
        out["visions"] = sounds.visionReport;
        out["sound_bindings"] = Json{{"status", bindingReport.at("status")}, {"bound_variants", bindingReport.at("bound_variants")}, {"errors", bindingReport.at("errors")},
                                     {"report", "content_source/sounds.bindings.json"}};
        out["sound_curves"] = Json{{"status", curveReport.at("status")}, {"complete_variants", curveReport.at("complete_variants")},
                                   {"incomplete_variants", curveReport.at("incomplete_variants")}, {"errors", curveReport.at("errors")},
                                   {"report", "content_source/sounds.curves.json"}};
        // BO2 cannot load WaW's curves: report the set once, name a curve only when its error is audible
        std::vector<const Json*> approximated;
        for (const auto& [_, entry] : curveReport.at("curves").items())
            if (entry.at("status") != "exact_shape")
                approximated.push_back(&entry);
        if (!approximated.empty())
        {
            const Json* worst = approximated.front();
            long long uses = 0;
            for (const auto* e : approximated)
            {
                if ((*e)["nearest"]["rmsDbError"].get<double>() > (*worst)["nearest"]["rmsDbError"].get<double>())
                    worst = e;
                uses += (*e)["uses"].get<long long>();
            }
            warnings.push_back("SOUND_CURVES " + std::to_string(approximated.size()) + " of " + std::to_string(curveReport.at("curves").size()) +
                               " WaW falloff curves have no exact stock BO2 curve (" + std::to_string(uses) + " alias slots); worst " + (*worst)["source"].get<std::string>() +
                               " -> " + (*worst)["nearest"]["t6"].get<std::string>() + " at " + Fixed((*worst)["nearest"]["rmsDbError"].get<double>(), 1) +
                               " dB RMS. List: content_source/sounds.curves.json");
        }
        for (const auto* entry : approximated)
        {
            const auto& nearest = (*entry)["nearest"];
            if (nearest["rmsDbError"].get<double>() > 3.0 || (*entry)["status"] == "UNSUPPORTED_SOUND_CURVE")
                warnings.push_back((*entry)["status"].get<std::string>() + " " + (*entry)["source"].get<std::string>() + ": " + std::to_string((*entry)["uses"].get<long long>()) +
                                   " alias curve slot(s); nearest stock T6 curve " + nearest["t6"].get<std::string>() + " (" + Fixed(nearest["rmsDbError"].get<double>(), 1) +
                                   " dB RMS, max gain error " + Fixed(nearest["maxError"].get<double>(), 3) + ")");
        }
    }
}
