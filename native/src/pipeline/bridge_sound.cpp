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

        Json BindBridgeCurves(const fs::path& content, const Json& sounds)
        {
            const auto driver = OptionalPath(sounds.at("t6_sound_driver")).value_or(fs::path{});
            try
            {
                return sound::BindCurves(content, driver, sounds.value("approximate_sound_curves", false));
            }
            catch (const sound::SoundError& error)
            {
                throw StepError("SoundError", error.what());
            }
        }

    } // namespace

    void StageBridgeSound(const Json& request, Json& report, extract::StockWaw* stock)
    {
        const auto& sounds = request.at("sounds");
        const auto project = request.at("project").get<std::string>();
        const auto projectRoot = files::FromUtf8(request.at("project_root").get<std::string>());
        const auto roots = PathList(request.at("roots"));
        const auto content = projectRoot / "content_source";
        auto& warnings = report["warnings"];
        auto& out = report["content"];
        // source_amb_script: the WaW map's clientscripts/<map>_amb.csc
        std::optional<fs::path> sourceAmb;
        if (sounds.at("map_stem").is_string())
            for (const auto& root : PathList(sounds.at("amb_roots")))
            {
                const auto candidate = root / "clientscripts" / files::FromUtf8(sounds["map_stem"].get<std::string>() + "_amb.csc");
                if (mod::IsFile(candidate))
                {
                    sourceAmb = candidate;
                    break;
                }
            }
        const auto ents = files::FromUtf8(sounds.at("ents").get<std::string>());
        const auto found = Timed("Resolve sound dependencies", [&] {
            return DiscoverSounds({projectRoot, roots, stock, sounds.at("zone_sounds").get<std::vector<std::string>>(),
                                   sounds.at("compiled_sounds").get<std::vector<std::string>>(), ents, sourceAmb});
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
        sound::StageInput input;
        for (const auto& r : soundRoots)
            input.roots.push_back(Str(r));
        for (const auto& d : sounds.at("iwd_dirs"))
            input.iwdDirs.push_back(d.get<std::string>());
        input.names = std::set<std::string>();
        input.names->insert(found.soundNames.begin(), found.soundNames.end());
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
                input.globalsFile = Str(direct.front());
                break;
            }
        }
        if (!input.globalsFile && stock)
            if (const auto hit = stock->RootFor("snddriverglobals", "singleton"))
                input.globalsFile = Str(hit->second / "soundglobals/singleton.w2bsndglobals.json");
        input.project = content;
        fs::create_directories(content);
        Timed("Stage sound sources", [&] { return sound::Stage(input); });
        const auto soundReport = mod::Json(content / "sounds.stage.json");
        audio::Tools tools;
        if (sounds.at("xwma_helper").is_string())
            tools.xwmaHelper = files::FromUtf8(sounds["xwma_helper"].get<std::string>());
        if (sounds.at("resampler").is_string())
            tools.resampler = files::FromUtf8(sounds["resampler"].get<std::string>());
        const auto decoder = sounds.at("audio_decoder").is_string() ? sounds["audio_decoder"].get<std::string>() : std::string();
        Timed("Decode audio", [&] { return audio::Stage(content, content / "pcm", decoder, tools); });
        const auto audioReport = mod::Json(content / "pcm" / "audio.stage.json");
        const auto bindingReport = Timed("Bind decoded audio", [&] { return sound::BindPcm(content, content / "pcm"); });
        out["animation_sound_identities"] = found.animationSoundIdentities;
        const auto curveReport = Timed("Bind sound curves", [&] { return BindBridgeCurves(content, sounds); });
        const auto bank = "waw_" + project + ".all";
        const auto mix = Timed("Build sound bank", [&] {
            return SoundMix({projectRoot, project, OptionalPath(request.at("bo2_root")), stock, found.scriptSounds, found.entitySounds, sourceAmb,
                             tools.resampler ? Str(*tools.resampler) : std::string()});
        });
        mix.report.MergeInto(report);
        const auto& budget = mix.budget;
        const auto& bankReport = mix.bankReport;
        WriteAmbCsc(project, projectRoot, sourceAmb, ents).MergeInto(report);
        if (!budget.is_null())
        {
            const auto mb = [&](const char* key) { return std::to_string(budget.at(key).get<long long>() / 1048576); };
            warnings.push_back("SOUND_LOADED_TO_STREAMED " + std::to_string(budget.at("streamed_files").get<long long>()) + " WaW loaded sounds streamed: loaded-bank budget " +
                               mb("bytes") + " MB (largest stock map bank " + budget.at("reference_bank").get<std::string>() + " minus the template bank's " + mb("template_bytes") +
                               " MB); kept " + std::to_string(budget.at("loaded_files").get<long long>()) + " loaded files, weapon dependencies, then played sounds (" + mb("loaded_bytes") +
                               " MB); " + std::to_string(budget.at("played_files_streamed").get<long long>()) +
                               " played files still stream (10 stream voices). List: content_source/sounds.bank.json");
            const auto pool = budget.value("entry_pool", Json::object());
            if (!pool.empty() && pool.at("always_loaded").get<long long>() + pool.at("template").get<long long>() + pool.value("map", 0LL) > pool.at("size").get<long long>())
                warnings.push_back("SOUND_ENTRY_POOL " + IntDictRepr(pool) + ": T6 cannot load the map bank (entry pool full)");
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
        out["visions"] = sounds.at("vision_report");
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
