#pragma once

#include "core/text.h"

namespace w2b::weapons
{
    // A rigid skeleton can combine coincident roots while preserving global transforms;
    // an animated one keeps its root bones.
    // Returns the names of the merged roots for the report.
    util::Json PrepareSkeleton(util::Json& gltf, bool rigid);
    // The root joints of each skin.
    std::vector<std::vector<std::size_t>> SkinRoots(const util::Json& gltf);
    std::size_t LodVertices(const util::Json& gltf);
    bool RebindToRoot(util::Json& gltf);
}
