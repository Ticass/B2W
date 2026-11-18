#pragma once

#include <string>
#include <string_view>

namespace w2b::hash
{
    // hashlib.sha256(data).hexdigest()
    std::string Sha256Hex(std::string_view data);
    // hashlib.sha1(data).hexdigest()
    std::string Sha1Hex(std::string_view data);
} // namespace w2b::hash
