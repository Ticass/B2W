#pragma once
// t6bridge.stage_effects: the effects the map's scripts load, their closure,
// effect materials, world-technique materials and the converted effects.

#include "core/pytext.h"

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    // request: the stage_effects arguments (see t6bridge._stage_effects_native);
    // returns the report additions and the EffectsResult fields; "recovered"
    // lists the IWD images recovery decoded for it
    py::Json StageEffectsStep(const py::Json& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline
