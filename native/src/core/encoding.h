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

    // bytes.decode("utf-8", errors="replace"): each maximal invalid
    // subsequence becomes U+FFFD, as CPython's decoder replaces it
    std::string Utf8Replace(std::string_view bytes);

    // "utf-8-sig": the same, without a leading BOM
    std::string Utf8SigReplace(std::string_view bytes);

    // universal newlines (text mode reads): "\r\n" and "\r" -> "\n"
    std::string UniversalNewlines(std::string_view text);

    // wawtext.read: UTF-8 if valid, else cp1252 with replacement; universal newlines
    std::string WawText(std::string_view bytes);
} // namespace w2b::encoding
