#pragma once
// t6bridge.stage_bridge from the model overlay effects to the written report:
// model and material identities, the map scripts' link check, WaW's in-game
// menus and their art, localization, surface fixes, rope runtime, rumble,
// the map zone, framework coverage and bridge_stage.report.json. Served by
// the IWD recovery (menu art resolves through it).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    // request: the report so far and the stage_bridge values this part reads
    // (see t6bridge._bridge_tail_native); returns {report, stock_prepared, recovered}
    py::Json BridgeTail(const py::Json& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline
