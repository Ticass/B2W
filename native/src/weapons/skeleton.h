#pragma once

#include "core/text.h"

namespace w2b::weapons
{
    // A rigid skeleton can combine coincident roots while preserving global transforms.
    // Returns the names of the merged roots for the report.
    util::Json PrepareSkeleton(util::Json& gltf, bool rigid);
    std::size_t LodVertices(const util::Json& gltf);
    bool RebindToRoot(util::Json& gltf);
}
