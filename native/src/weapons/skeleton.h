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
    // Orders the LOD's sibling mesh nodes skinned first so the mod tools linker's 16-bit surface
    // vertex bases fit (skinned surfaces end within animatedLimit, rigid ones within rigidLimit).
    // False, with the glTF unchanged, when they cannot fit or the layout is not one it can reorder.
    bool OrderSurfacesForLinker(util::Json& gltf, std::size_t animatedLimit, std::size_t rigidLimit);
}
