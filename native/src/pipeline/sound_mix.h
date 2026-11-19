#pragma once
// t6bridge.stage_bridge, the sound mix: the stock WaW and BO2 measurements
// (audioremaster stock profiles and classes, bus groups, reverb send, the
// loaded-bank budget) and the bank written with them (sound/bank.cpp).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, project, bo2_root, stock_waw, script_sounds,
    // entity_sounds, source_amb}; returns {warnings, content, bank_report,
    // budget, reverb_send, stock_prepared}
    py::Json SoundMix(const py::Json& request);
} // namespace w2b::pipeline
