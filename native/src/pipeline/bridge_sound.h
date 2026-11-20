#pragma once

#include "core/text.h"
#include "pipeline/bridge_tail.h"

namespace w2b::pipeline
{
    // the bridge's sounds: dependencies, sources, decoded audio, curves, the bank and the ambient script
    void StageBridgeSound(const TailInput& input, BridgeReport& report, extract::StockWaw* stock);
}
