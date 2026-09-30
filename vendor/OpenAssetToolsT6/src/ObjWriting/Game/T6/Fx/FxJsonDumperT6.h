#pragma once

#include "Dumping/AbstractAssetDumper.h"
#include "Game/T6/T6.h"

namespace fx
{
    // waw2bo2: lossless dump of a compiled T6 FxEffectDef (fx/<name>.w2bfx.json),
    // the same format fx::CreateJsonLoaderT6 reads. Used as ground truth for the
    // WaW -> BO2 FX translation and for round-trip checks of the loader.
    class JsonDumperT6 final : public AbstractAssetDumper<T6::AssetFx>
    {
    public:
        explicit JsonDumperT6(const AssetPool<T6::AssetFx::Type>& pool);

    protected:
        void DumpAsset(AssetDumpingContext& context, const XAssetInfo<T6::AssetFx::Type>& asset) override;
    };
} // namespace fx
