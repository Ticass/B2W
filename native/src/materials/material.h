#pragma once
// A WaW material converted onto a stock BO2 donor material: the donor's
// technique and slots, the WaW material's images, constants and render state.

#include "core/pytext.h"
#include "materials/waw_material.h"

namespace w2b::materials
{
    // the WaW technique the material draws with, refined by its unlit state
    std::string SourceTechnique(const WawMaterial& source);
    // the T6 material file: `donor` with the WaW material's inputs bound
    py::Json BuildMaterial(const WawMaterial& source, const py::Json& donor, std::vector<std::string>& notes);
} // namespace w2b::materials
