#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace w2b::assets
{
    std::filesystem::path MaterialPath(std::string name);
    std::filesystem::path ImagePath(std::string name, std::string_view extension);
}
