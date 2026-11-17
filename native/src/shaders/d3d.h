#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace w2b::shaders
{
    class ShaderError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // shaders.disassemble: D3DDisassemble(flags 0), text up to its NULs.
    std::string Disassemble(std::string_view bytecode);

    // shaders.compile_hlsl: D3DCompile(entry "main", strict + IEEE strict).
    std::string CompileHlsl(std::string_view source, std::string_view profile);
} // namespace w2b::shaders
