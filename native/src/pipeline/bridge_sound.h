#pragma once

#include "core/pytext.h"

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    // the bridge's sounds: dependencies, sources, decoded audio, curves, the bank and the ambient script
    void StageBridgeSound(const py::Json& request, py::Json& report, extract::StockWaw* stock);
}
