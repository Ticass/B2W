#pragma once
// The file form of a WaW material, for the sources and readers that still
// use OpenAssetTools' material files during the move to objects.

#include "core/pytext.h"
#include "materials/waw_material.h"

namespace w2b::materials
{
    // the material of an OpenAssetTools T4 material file
    WawMaterial FromOatJson(const py::Json& data);
    // the file OpenAssetTools writes for the material
    py::Json ToOatJson(const WawMaterial& material);
    // one render state / texture of it as that file holds it
    py::Json ToOatJson(const WawStateBits& state);
    py::Json ToOatJson(const WawTexture& texture);
} // namespace w2b::materials
