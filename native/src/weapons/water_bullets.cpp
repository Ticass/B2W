#include "weapons/water_bullets.h"

#include "core/encoding.h"
#include "core/files.h"
#include "weapons/weapon.h"
#include "weapons/water_bullets_script.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace w2b::weapons
{
    namespace
    {
        constexpr std::string_view penetrationTable = "info/bullet_penetration_sp";
        constexpr std::array<std::string_view, 10> spreadFields{
            "hipSpreadStandMin", "hipSpreadDuckedMin", "hipSpreadProneMin", "hipSpreadMax", "hipSpreadDuckedMax",
            "hipSpreadProneMax", "hipSpreadDecayRate", "hipSpreadFireAdd", "hipSpreadMoveAdd", "adsSpread"};

        std::vector<std::pair<std::string, double>> WaterDepths(const path& source)
        {
            std::string bytes;
            if (!files::ReadBytes(source, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(source));
            const auto text = encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
            const auto parts = util::Split(util::Strip(text), "\\");
            std::map<std::string_view, std::string_view> fields;
            for (std::size_t i = 1; i + 1 < parts.size(); i += 2)
                fields[parts[i]] = parts[i + 1];
            std::vector<std::pair<std::string, double>> depths;
            for (const std::string_view kind : {"small", "medium", "large"})
            {
                const auto field = fields.find(std::string(kind) + "_water");
                if (field == fields.end())
                    continue;
                double depth = 0;
                if (!util::ParseFloat(field->second, depth))
                    throw WeaponError("invalid water penetration depth: " + util::StringRepr(field->second));
                depths.emplace_back(kind, depth);
            }
            return depths;
        }

        double Number(const InfoFields& fields, std::string_view field)
        {
            double value = 0;
            return util::ParseFloat(Field(fields, field), value) ? value : 0;
        }

        void AppendNumber(std::string& text, double value)
        {
            std::array<char, 32> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                                    std::chars_format::general, 6);
            if (error != std::errc{})
                throw std::runtime_error("cannot format water bullet field");
            text.append(buffer.data(), end);
        }

        std::string WeaponRow(const std::string& name, const InfoFields& fields, double depth)
        {
            std::string row = "    level.waw_water_bullets[\"" + name + "\"] = waw_water_row( ";
            for (const auto field : spreadFields)
            {
                AppendNumber(row, Number(fields, field));
                row += ", ";
            }
            const auto shots = Number(fields, "shotCount");
            if (!std::isfinite(shots))
                throw WeaponError("invalid shot count for " + name);
            AppendNumber(row, std::max(1.0, std::trunc(shots)));
            row += ", ";
            AppendNumber(row, depth);
            return row + " );\n";
        }

        WaterBulletRows StageRows(const path& root, const std::vector<std::string>& weaponNames, const path& source, std::string& rows)
        {
            WaterBulletRows staged{WaterDepths(source), {}};
            const std::set<std::string> targets(weaponNames.begin(), weaponNames.end());
            for (const auto& name : targets)
            {
                const auto weapon = root / "content_source/weapons" / files::FromUtf8(name);
                if (!files::fs::is_regular_file(weapon))
                    continue;
                const auto fields = ReadInfo(weapon);
                const auto& type = Field(fields, "penetrateType");
                const auto found = std::ranges::find(staged.depths, type, &std::pair<std::string, double>::first);
                const auto depth = found == staged.depths.end() ? 0.0 : found->second;
                if (Field(fields, "weaponType") != "bullet" || depth <= 0)
                    continue;
                staged.weapons.push_back(name);
                rows += WeaponRow(name, fields, depth);
            }
            return staged;
        }

        path FindPenetrationTable(const std::vector<path>& roots)
        {
            for (const auto& root : roots)
            {
                const auto source = root / files::FromUtf8(penetrationTable);
                if (files::fs::exists(source))
                    return source;
            }
            return {};
        }
    }

    Json WaterBulletRows::ToJson() const
    {
        auto byType = Json::object();
        for (const auto& [type, depth] : depths)
            byType[type] = depth;
        return {{"depths", std::move(byType)}, {"weapons", weapons}};
    }

    WaterBulletStage StageWaterBullets(const path& root, const std::vector<path>& roots, bool hasWater, const std::vector<std::string>& weapons)
    {
        WaterBulletStage result;
        std::string rows;
        if (hasWater)
        {
            const auto source = FindPenetrationTable(roots);
            if (source.empty())
                result.warning = "WATER_BULLETS_NO_TABLE " + std::string(penetrationTable) +
                                    " in no source root; bullets stop at the water";
            else
                result.rows = StageRows(root, weapons, source, rows);
        }

        std::string script = "// waw2bo2: no water brushes or no bullet weapon penetrates water.\n\ninit()\n{\n}\n";
        if (!rows.empty())
        {
            constexpr std::string_view marker = "    // WEAPON_TABLE\n";
            script = waterBulletsScript;
            script.replace(script.find(marker), marker.size(), rows);
        }
        files::WriteText(root / "maps/mp/waw/_waw2bo2_water_bullets.gsc", script);
        return result;
    }
}
