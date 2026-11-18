#pragma once

#include "core/pytext.h"

namespace w2b::textures { class IwdRecovery; }

namespace w2b::pipeline
{
    py::Json StageBridgeImages(const py::Json& request, textures::IwdRecovery& recovery);
}
