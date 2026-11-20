#pragma once

#include "core/text.h"

#include <array>
#include <filesystem>

namespace w2b::sound
{
    using Point = std::array<double, 2>;

    struct Curve
    {
        std::string name;
        int index;
        std::vector<Point> points;
    };

    std::vector<Curve> ReadDriverCurves(const std::filesystem::path& driver);
    util::Json TranslateCurves(const util::Json& sources, const std::vector<Curve>& stock, bool approximate);
}
