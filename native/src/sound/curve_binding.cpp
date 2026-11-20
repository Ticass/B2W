#include "sound/bindings.h"

#include "core/files.h"
#include "core/parallel.h"
#include "sound/curves.h"
#include "sound/stage.h"

#include <cwctype>
#include <format>
#include <map>
#include <set>

namespace w2b::sound
{
    namespace
    {
        namespace fs = std::filesystem;
        using util::Json;
        constexpr std::pair<std::string_view, std::string_view> CurveFields[] = {
            {"volumeFalloffCurve", "DryMaxCurve"}, {"volumeMinFalloffCurve", "DryMinCurve"},
            {"reverbFalloffCurve", "WetMaxCurve"}, {"reverbMinFalloffCurve", "WetMinCurve"}
        };

        struct Document
        {
            fs::path path;
            Json value;
        };

        std::vector<Document> ReadBindings(const fs::path& project, const Json& aliases)
        {
            std::set<std::wstring> seen;
            std::vector<Document> documents;
            documents.reserve(aliases.size());
            for (const auto& item : aliases)
            {
                auto path = project / files::FromUtf8(item.at("output").get_ref<const std::string&>());
                auto key = path.make_preferred().wstring();
                std::ranges::transform(key, key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (seen.insert(std::move(key)).second)
                    documents.push_back({std::move(path), {}});
            }
            parallel::For(documents.size(), [&](std::size_t i)
            {
                auto& document = documents[i];
                document.value = Json::parse(files::ReadText(document.path));
            }, 4);
            return documents;
        }

        Json UsedCurves(const std::vector<Document>& documents)
        {
            std::map<std::string, const Json*> used;
            for (const auto& document : documents)
                for (const auto& row : document.value.at("aliases"))
                    for (const auto& [key, field] : CurveFields)
                    {
                        const auto& curve = row.at("curves").at(key);
                        used.try_emplace(curve.at("name").get_ref<const std::string&>(), &curve);
                    }
            auto ordered = Json::array();
            for (const auto& [name, curve] : used)
                ordered.push_back(*curve);
            return ordered;
        }

        void BindRows(std::vector<Document>& documents, Json& report)
        {
            auto& table = report.at("curves");
            std::size_t complete = 0, incomplete = 0;
            for (auto& document : documents)
                for (auto& row : document.value.at("aliases"))
                {
                    auto fields = Json::object();
                    auto missing = Json::array();
                    for (const auto& [key, field] : CurveFields)
                    {
                        const auto binding = table.find(row.at("curves").at(key).at("name").get_ref<const std::string&>());
                        if (binding == table.end())
                        {
                            missing.push_back(key);
                            continue;
                        }
                        auto& uses = binding->at("uses");
                        uses = uses.get<std::size_t>() + 1;
                        if (binding->contains("index"))
                            fields[field] = {{"t6", binding->at("t6")}, {"index", binding->at("index")},
                                {"status", binding->at("status")}, {"maxError", binding->at("maxError")},
                                {"rmsDbError", binding->at("rmsDbError")}};
                        else
                            missing.push_back(key);
                    }
                    row["t6_curves"] = std::move(fields);
                    if (missing.empty())
                    {
                        row.erase("t6_curves_missing");
                        ++complete;
                    }
                    else
                    {
                        row["t6_curves_missing"] = std::move(missing);
                        ++incomplete;
                    }
                }
            report["complete_variants"] = complete;
            report["incomplete_variants"] = incomplete;
        }
    }

    util::Json BindCurves(const fs::path& project, const fs::path& driver, bool approximate)
    {
        const auto bindings = Json::parse(files::ReadText(project / "sounds.bindings.json"));
        Json report{{"status", "curves_bound_semantic_ir_not_T6_bank"}, {"approximation", approximate},
                    {"approximation_metric", "RMS dB over normalized distance; -60 dB floor; preserve smooth silent tails"},
                    {"driver_replacement", "impossible: BO2 rejects a second snddriverglobals asset (ERR_DROP)"},
                    {"curves", Json::object()}, {"complete_variants", 0}, {"incomplete_variants", 0},
                    {"errors", Json::array()}};
        const auto output = project / "sounds.curves.json";
        const auto driverName = driver.empty() ? std::string("None") : files::Utf8(fs::path(driver).make_preferred());
        if (driver.empty() || !fs::is_regular_file(driver))
        {
            report["status"] = "missing_t6_sound_driver";
            report["errors"].push_back({{"reason", "stock T6 sound driver sidecar not found: " + driverName}});
            files::WriteText(output, util::JsonDumps(report, 2) + "\n");
            return report;
        }
        const auto stock = ReadDriverCurves(driver);
        report["t6_driver"] = driverName;
        auto documents = ReadBindings(project, bindings.at("aliases"));
        try
        {
            report["curves"] = TranslateCurves(UsedCurves(documents), stock, approximate);
        }
        catch (const SoundError& error)
        {
            report["errors"].push_back({{"reason", error.what()}});
        }
        for (auto& curve : report.at("curves"))
            curve["uses"] = 0;
        BindRows(documents, report);
        parallel::For(documents.size(), [&](std::size_t i)
        {
            files::WriteText(documents[i].path, util::JsonDumps(documents[i].value, 2) + "\n");
        }, 4);
        for (const auto& [name, curve] : report.at("curves").items())
        {
            if (curve.at("status") != "UNSUPPORTED_SOUND_CURVE")
                continue;
            const auto& nearest = curve.at("nearest");
            const auto reason = std::format("no stock T6 curve has this shape and BO2 cannot load a replacement "
                                            "driver; nearest {} differs by {:.4f}",
                                            nearest.at("t6").get_ref<const std::string&>(), nearest.at("maxError").get<double>());
            report["errors"].push_back({{"curve", name}, {"uses", curve.at("uses")}, {"reason", reason}});
        }
        files::WriteText(output, util::JsonDumps(report, 2) + "\n");
        return report;
    }
}
