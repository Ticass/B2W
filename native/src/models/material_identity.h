#pragma once

#include "models/models.h"

namespace w2b::models
{
    Json IsolateMaterialNames(const Path& project, const Path& stock, const Json& materials, const Json& hudNames);
    Json IsolateMaterialNamesRequest(const Path& request);
}
