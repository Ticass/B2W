#pragma once
// Text exactly as the Python converter writes it, so native steps produce the
// same bytes: float repr, json.dumps(..., indent=N) with ensure_ascii, and the
// str methods the converter relies on.

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace w2b::py
{
    using Json = nlohmann::ordered_json;

    // repr(float): shortest round-trip digits, Python's fixed/exponent choice.
    std::string FloatRepr(double value);

    // json.dumps(value, indent=indent) (ensure_ascii, ", " / ": " separators).
    std::string JsonDumps(const Json& value, int indent);

    // str.splitlines() for ASCII-range line boundaries.
    std::vector<std::string_view> SplitLines(std::string_view text);

    // str.strip() / str.rstrip(chars) for ASCII whitespace.
    std::string_view Strip(std::string_view text);
    std::string_view RStrip(std::string_view text, std::string_view chars);

    // str.split(sep) (sep non-empty).
    std::vector<std::string_view> Split(std::string_view text, std::string_view sep);

    // float(text) / int(text) for the forms Python accepts; false if Python raises ValueError.
    bool ParseFloat(std::string_view text, double& value);
    bool ParseInt(std::string_view text, long long& value, bool& overflow);

    bool IsSpace(char c);
    bool IsDigit(char c);
    bool IsWord(char c);
} // namespace w2b::py
