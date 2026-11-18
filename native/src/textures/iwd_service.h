#pragma once

#include "core/pytext.h"

#include <functional>
#include <iosfwd>
#include <map>
#include <string>

namespace w2b::textures
{
    class IwdRecovery;
    // a staging step served beside the recovery, sharing its decoded images
    using RecoveryOperation = std::function<py::Json(const py::Json& request, IwdRecovery& recovery)>;

    void ServeIwdRecovery(std::istream& input, std::ostream& output, const std::map<std::string, RecoveryOperation>& operations = {});
}
