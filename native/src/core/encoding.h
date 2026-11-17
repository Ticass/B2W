#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace w2b::encoding
{
    class DecodeError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // bytes.decode("cp1252") (strict, as Path.read_text() decodes on this
    // system's locale), returned as UTF-8
    std::string Cp1252ToUtf8(std::string_view bytes);

    // bytes.decode("cp437"), as zipfile decodes names without the UTF-8 flag
    std::string Cp437ToUtf8(std::string_view bytes);

    void AppendUtf8(std::string& out, unsigned codepoint);
} // namespace w2b::encoding
