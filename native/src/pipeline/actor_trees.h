#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    std::vector<std::string> NativeTreeAnimations(std::string_view tree);
    py::Json StageOwnedActorTree(const py::Json& request);
    py::Json StageOwnedActorTreeRequest(const std::filesystem::path& request);
}
