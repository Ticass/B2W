#pragma once

#include "Asset/IAssetCreator.h"
#include "Game/T6/T6.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"

#include <memory>

namespace fx
{
    // waw2bo2: builds a T6 FxEffectDef from fx/<name>.w2bfx.json (the format
    // fx::JsonDumperT6 writes). Materials, models and referenced effects are
    // loaded as dependencies.
    std::unique_ptr<AssetCreator<T6::AssetFx>> CreateJsonLoaderT6(MemoryManager& memory, ISearchPath& searchPath);
} // namespace fx
