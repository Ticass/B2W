#pragma once

#include "core/pytext.h"

namespace w2b::weapons
{
    // A rigid skeleton can combine coincident roots while preserving global transforms.
    // Returns the names of the merged roots for the report.
    py::Json PrepareSkeleton(py::Json& gltf, bool rigid);
    std::size_t LodVertices(const py::Json& gltf);
    bool RebindToRoot(py::Json& gltf);
}
