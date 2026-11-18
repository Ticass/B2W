#pragma once

#include "paths/paths.h"

#include <map>

namespace w2b::paths
{
    class Aliases
    {
    public:
        explicit Aliases(const std::optional<Names>& substates);
        Alias Select(std::string_view source, std::optional<double> height) const;

    private:
        using Heights = std::map<int, std::string>;
        Alias Measured(std::string_view name, std::optional<double> height, std::optional<double> number) const;
        const std::optional<Names>& substates_;
        Heights down_;
        Heights up_;
    };
}
