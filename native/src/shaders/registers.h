#pragma once
// SM3 register syntax, matched with regular expressions:
//   REG = ^(r\d+|v\d+|c\d+|oC\d+|o\d+|oDepth)(?:\.([xyzwrgba]{1,4}))?$
// Hand-written matchers with the same alternation/backtracking results.

#include <string>
#include <string_view>
#include <vector>

namespace w2b::shaders
{
    struct Register
    {
        std::string reg;
        std::string swizzle; // without the dot
        bool hasSwizzle = false;
    };

    // REG.fullmatch(text)
    bool MatchRegister(std::string_view text, Register& out);

    // re.fullmatch(r'(r\d+|v\d+|c\d+)_abs(\.[xyzwrgba]{1,4})?', text): base register, ".swz" or ""
    bool MatchAbsModifier(std::string_view text, std::string& base, std::string& swizzle);

    // SEM.fullmatch: ^[A-Z][A-Z0-9_]*$
    bool MatchSemantic(std::string_view text);

    // re.findall(r'\b(?:r\d+|v\d+|c\d+|oC\d+|o\d+|oDepth)(?=[._,\s)]|$)', text)
    std::vector<std::string> FindRegisters(std::string_view text);

    // str.translate(SWIZZLE): rgba -> xyzw
    std::string SwizzleXyzw(std::string_view swizzle);
} // namespace w2b::shaders
