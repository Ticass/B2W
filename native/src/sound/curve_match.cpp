#include "sound/curves.h"

#include "sound/stage.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace w2b::sound
{
    namespace
    {
        using util::Json;
        constexpr double Tolerance = 1e-5;
        constexpr std::size_t Samples = 256;

        double Gain(const Curve& curve, double x, bool left)
        {
            for (std::size_t i = 1; i < curve.points.size(); ++i)
            {
                const auto [x0, y0] = curve.points[i - 1];
                const auto [x1, y1] = curve.points[i];
                if (x < x1 || (left && x == x1))
                    return x1 == x0 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
            }
            return curve.points.back()[1];
        }

        class SampledCurve
        {
        public:
            explicit SampledCurve(const Curve& curve) : curve(curve)
            {
                const auto& data = curve.points;
                if (data.size() < 2 || data.front()[0] != 0 || data.back()[0] != 1)
                    throw SoundError("invalid curve " + curve.name);
                for (std::size_t i = 1; i < data.size(); ++i)
                    if (data[i][0] < data[i - 1][0] || !std::isfinite(data[i][0]) || !std::isfinite(data[i][1]))
                        throw SoundError("invalid curve " + curve.name);
                silent = data.back()[1] == 0 && Gain(curve, 1, true) == 0;
                for (std::size_t i = 0; i < Samples; ++i)
                    gains[i] = std::max(Gain(curve, (static_cast<double>(i) + 0.5) / Samples, false), 0.001);
            }

            const Curve& curve;
            std::array<double, Samples> gains{};
            bool silent = false;
        };

        struct Score
        {
            const SampledCurve* target;
            double maxError, rmsDb;
            std::string_view endpoint;

            bool Exact() const { return maxError <= Tolerance && endpoint != "differs"; }
            Json Fields() const { return {{"maxError", maxError}, {"endpoint", endpoint}, {"rmsDbError", rmsDb}}; }
        };

        Score Compare(const SampledCurve& source, const SampledCurve& target)
        {
            const auto& a = source.curve;
            const auto& b = target.curve;
            std::set<double> breaks;
            for (const auto& p : a.points)
                breaks.insert(p[0]);
            for (const auto& p : b.points)
                breaks.insert(p[0]);
            const auto error = [&](double x, bool left)
            {
                return std::abs(Gain(a, x, left) - Gain(b, x, left));
            };
            double max = 0;
            for (auto it = breaks.begin(), next = std::next(it); next != breaks.end(); ++it, ++next)
            {
                const auto between = (*it + *next) / 2;
                max = std::max({max, error(*it, false), error(*it, true), error(between, false), error(between, true)});
            }
            const bool left = error(1, true) <= Tolerance;
            const bool right = error(1, false) <= Tolerance;
            double sum = 0;
            for (std::size_t i = 0; i < Samples; ++i)
            {
                const auto db = 20 * std::log10(source.gains[i] / target.gains[i]);
                sum += std::pow(db, 2.0);
            }
            return {&target, max, std::sqrt(sum / Samples), left != right ? "evaluator_dependent" : left ? "equal" : "differs"};
        }

        Json Match(const Curve& curve, const std::vector<SampledCurve>& stock, bool approximate)
        {
            const SampledCurve source(curve);
            std::vector<Score> scored;
            scored.reserve(stock.size());
            for (const auto& target : stock)
                scored.push_back(Compare(source, target));
            const auto exact = std::ranges::min_element(scored, {}, [&](const Score& s)
            {
                return std::tuple{!s.Exact(), s.target->curve.name != curve.name, s.target->curve.index};
            });
            const auto nearest = std::ranges::min_element(scored, {}, [&](const Score& s)
            {
                return std::tuple{source.silent && !s.target->silent, s.rmsDb, s.maxError, s.target->curve.index};
            });
            const auto& target = nearest->target->curve;
            Json closest{{"t6", target.name}, {"index", target.index}};
            closest.update(nearest->Fields());
            Json entry{{"source", curve.name}, {"sourceIndex", curve.index}, {"nearest", closest}};
            if (exact->Exact() || approximate)
            {
                const auto& selected = exact->Exact() ? *exact : *nearest;
                entry["status"] = exact->Exact() ? "exact_shape" : "APPROXIMATED_SOUND_CURVE";
                entry["t6"] = selected.target->curve.name;
                entry["index"] = selected.target->curve.index;
                entry.update(selected.Fields());
                if (exact->Exact() && selected.endpoint == "evaluator_dependent")
                    entry["note"] = "value at x=1 depends on the T6 evaluator at a vertical step (unmeasured)";
            }
            else
                entry["status"] = "UNSUPPORTED_SOUND_CURVE";
            return entry;
        }
    }

    util::Json TranslateCurves(const Json& sources, const std::vector<Curve>& stock, bool approximate)
    {
        if (stock.empty())
            throw SoundError("T6 sound driver contains no falloff curves");
        auto results = Json::object();
        if (sources.empty())
            return results;
        std::vector<SampledCurve> targets;
        targets.reserve(stock.size());
        for (const auto& curve : stock)
            targets.emplace_back(curve);
        for (const auto& source : sources)
        {
            auto points = source.at("points").get<std::vector<Point>>();
            const auto length = static_cast<long long>(points.size());
            const auto count = source.value("pointCount", length);
            points.resize(static_cast<std::size_t>(count < 0 ? std::max(0LL, length + count) : std::min(count, length)));
            Curve curve{source.at("name").get<std::string>(), source.at("index").get<int>(), std::move(points)};
            results[curve.name] = Match(curve, targets, approximate);
        }
        return results;
    }
}
