#pragma once

#include "core/text.h"

#include <string>
#include <string_view>

namespace w2b::shaders
{
    struct Translation
    {
        std::string hlsl;
        util::Json report; // the dict shaders._translate returns, in its key order
    };

    // shaders._translate(assembly, bindings): SM3 assembly -> SM5 HLSL.
    // bindings == nullptr is the unbound validation ABI. Throws ShaderError
    // with a message on anything it cannot translate.
    Translation Translate(std::string_view assembly, const util::Json* bindings);
} // namespace w2b::shaders
