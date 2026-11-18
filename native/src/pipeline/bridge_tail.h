#pragma once
// The bridge's final staging passes, from BO2 script templates through the
// written map zone and report. The IWD service supplies image recovery.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    // request: the report so far and the stage_bridge values this part reads
    // (see stage_bridge in t6bridge.py); returns {report, stock_prepared, recovered}
    py::Json BridgeTail(const py::Json& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline
