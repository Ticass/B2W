#pragma once

#include "core/pytext.h"

namespace w2b::materials
{
    std::string SourceTechnique(const py::Json& source);
    py::Json BuildMaterial(const py::Json& source, const py::Json& donor, std::vector<std::string>& notes);
}
