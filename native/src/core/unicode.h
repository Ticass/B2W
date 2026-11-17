#pragma once
// Python str semantics on code points: the converter's regular expressions
// use \s, \w and \d, which match Unicode characters in Python 3.

#include <string>
#include <string_view>

namespace w2b::unicode
{
    std::u32string Decode(std::string_view utf8);
    std::string Encode(std::u32string_view text);

    bool IsSpace(char32_t c); // str.isspace / re \s
    bool IsDigit(char32_t c); // re \d (Unicode decimal digits)
    bool IsWord(char32_t c);  // re \w
} // namespace w2b::unicode
