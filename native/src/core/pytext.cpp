#include "pytext.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace w2b::py
{
    bool IsSpace(const char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '\x1c' || c == '\x1d' || c == '\x1e'
               || c == '\x1f';
    }

    bool IsDigit(const char c)
    {
        return c >= '0' && c <= '9';
    }

    bool IsWord(const char c)
    {
        return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    std::string FloatRepr(const double value)
    {
        if (std::isnan(value))
            return "nan";
        if (std::isinf(value))
            return value > 0 ? "inf" : "-inf";

        // shortest round-trip digits in scientific form: d[.ddd]e[+-]XX
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
        std::string_view text(buffer, static_cast<size_t>(result.ptr - buffer));
        std::string sign;
        if (!text.empty() && text[0] == '-')
        {
            sign = "-";
            text.remove_prefix(1);
        }
        const auto e = text.find('e');
        std::string digits;
        for (const auto c : text.substr(0, e))
            if (c != '.')
                digits += c;
        const int exponent = std::atoi(std::string(text.substr(e + 1)).c_str());
        // Python: value = 0.d1d2...dn * 10^decpt
        const int decpt = exponent + 1;
        const int n = static_cast<int>(digits.size());

        if (decpt <= -4 || decpt > 16)
        {
            std::string mantissa(1, digits[0]);
            if (n > 1)
                mantissa += "." + digits.substr(1);
            const int x = decpt - 1;
            std::string exp = std::to_string(x < 0 ? -x : x);
            if (exp.size() < 2)
                exp = "0" + exp;
            return sign + mantissa + "e" + (x < 0 ? "-" : "+") + exp;
        }
        if (decpt <= 0)
            return sign + "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
        if (decpt >= n)
            return sign + digits + std::string(static_cast<size_t>(decpt - n), '0') + ".0";
        return sign + digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
    }

    namespace
    {
        void AppendHex4(std::string& out, const unsigned value)
        {
            static constexpr char hex[] = "0123456789abcdef";
            out += "\\u";
            out += hex[(value >> 12) & 0xF];
            out += hex[(value >> 8) & 0xF];
            out += hex[(value >> 4) & 0xF];
            out += hex[value & 0xF];
        }

        // json.encoder.py_encode_basestring_ascii
        void AppendString(std::string& out, const std::string& text)
        {
            out += '"';
            for (size_t i = 0; i < text.size();)
            {
                const auto c = static_cast<unsigned char>(text[i]);
                if (c < 0x80)
                {
                    switch (c)
                    {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    case '\b':
                        out += "\\b";
                        break;
                    case '\f':
                        out += "\\f";
                        break;
                    default:
                        if (c < 0x20 || c == 0x7F) // json escapes everything outside ' '..'~'
                            AppendHex4(out, c);
                        else
                            out += static_cast<char>(c);
                    }
                    i++;
                    continue;
                }
                // decode one UTF-8 sequence
                unsigned codepoint = 0;
                int extra = 0;
                if ((c & 0xE0) == 0xC0)
                {
                    codepoint = c & 0x1F;
                    extra = 1;
                }
                else if ((c & 0xF0) == 0xE0)
                {
                    codepoint = c & 0x0F;
                    extra = 2;
                }
                else
                {
                    codepoint = c & 0x07;
                    extra = 3;
                }
                for (int k = 1; k <= extra && i + static_cast<size_t>(k) < text.size(); k++)
                    codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3F);
                i += static_cast<size_t>(extra) + 1;
                if (codepoint >= 0x10000)
                {
                    const unsigned v = codepoint - 0x10000;
                    AppendHex4(out, 0xD800 | (v >> 10));
                    AppendHex4(out, 0xDC00 | (v & 0x3FF));
                }
                else
                    AppendHex4(out, codepoint);
            }
            out += '"';
        }

        void Dump(std::string& out, const Json& value, const int indent, const int level)
        {
            const bool compact = indent < 0;
            switch (value.type())
            {
            case Json::value_t::null:
                out += "null";
                return;
            case Json::value_t::boolean:
                out += value.get<bool>() ? "true" : "false";
                return;
            case Json::value_t::number_integer:
                out += std::to_string(value.get<long long>());
                return;
            case Json::value_t::number_unsigned:
                out += std::to_string(value.get<unsigned long long>());
                return;
            case Json::value_t::number_float:
            {
                const auto v = value.get<double>();
                if (std::isnan(v))
                    out += "NaN";
                else if (std::isinf(v))
                    out += v > 0 ? "Infinity" : "-Infinity";
                else
                    out += FloatRepr(v);
                return;
            }
            case Json::value_t::string:
                AppendString(out, value.get_ref<const std::string&>());
                return;
            case Json::value_t::array:
            {
                if (value.empty())
                {
                    out += "[]";
                    return;
                }
                const std::string inner(compact ? 0 : static_cast<size_t>(indent * (level + 1)), ' ');
                out += compact ? "[" : "[\n";
                bool first = true;
                for (const auto& item : value)
                {
                    if (!first)
                        out += compact ? "," : ",\n";
                    first = false;
                    out += inner;
                    Dump(out, item, indent, level + 1);
                }
                if (!compact)
                    out += "\n" + std::string(static_cast<size_t>(indent * level), ' ');
                out += ']';
                return;
            }
            case Json::value_t::object:
            {
                if (value.empty())
                {
                    out += "{}";
                    return;
                }
                const std::string inner(compact ? 0 : static_cast<size_t>(indent * (level + 1)), ' ');
                out += compact ? "{" : "{\n";
                bool first = true;
                for (const auto& [key, item] : value.items())
                {
                    if (!first)
                        out += compact ? "," : ",\n";
                    first = false;
                    out += inner;
                    AppendString(out, key);
                    out += compact ? ":" : ": ";
                    Dump(out, item, indent, level + 1);
                }
                if (!compact)
                    out += "\n" + std::string(static_cast<size_t>(indent * level), ' ');
                out += '}';
                return;
            }
            default:
                out += "null";
            }
        }
    } // namespace

    std::string JsonDumps(const Json& value, const int indent)
    {
        std::string out;
        Dump(out, value, indent, 0);
        return out;
    }

    std::vector<std::string_view> SplitLines(const std::string_view text)
    {
        std::vector<std::string_view> lines;
        size_t start = 0;
        size_t i = 0;
        while (i < text.size())
        {
            const char c = text[i];
            if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '\x1c' || c == '\x1d' || c == '\x1e')
            {
                lines.push_back(text.substr(start, i - start));
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                    i++;
                i++;
                start = i;
                continue;
            }
            i++;
        }
        if (start < text.size())
            lines.push_back(text.substr(start));
        return lines;
    }

    std::string_view Strip(std::string_view text)
    {
        while (!text.empty() && IsSpace(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && IsSpace(text.back()))
            text.remove_suffix(1);
        return text;
    }

    std::string_view RStrip(std::string_view text, const std::string_view chars)
    {
        while (!text.empty() && chars.find(text.back()) != std::string_view::npos)
            text.remove_suffix(1);
        return text;
    }

    std::vector<std::string_view> Split(const std::string_view text, const std::string_view sep)
    {
        std::vector<std::string_view> parts;
        size_t start = 0;
        while (true)
        {
            const auto at = text.find(sep, start);
            if (at == std::string_view::npos)
            {
                parts.push_back(text.substr(start));
                return parts;
            }
            parts.push_back(text.substr(start, at - start));
            start = at + sep.size();
        }
    }

    bool ParseFloat(std::string_view text, double& value)
    {
        text = Strip(text);
        if (text.empty())
            return false;
        // Python's float() takes decimal forms, inf/infinity/nan, and '_' between digits;
        // never hex or nan(payload).
        std::string clean;
        for (size_t i = 0; i < text.size(); i++)
        {
            const char c = text[i];
            if (c == '_')
            {
                if (i == 0 || i + 1 >= text.size() || !IsDigit(text[i - 1]) || !IsDigit(text[i + 1]))
                    return false;
                continue;
            }
            if (c == 'x' || c == 'X' || c == '(' || c == ')' || IsSpace(c))
                return false;
            clean += c;
        }
        char* end = nullptr;
        errno = 0;
        value = std::strtod(clean.c_str(), &end);
        return end == clean.c_str() + clean.size();
    }

    bool ParseInt(std::string_view text, long long& value, bool& overflow)
    {
        text = Strip(text);
        overflow = false;
        value = 0;
        bool negative = false;
        if (!text.empty() && (text[0] == '+' || text[0] == '-'))
        {
            negative = text[0] == '-';
            text.remove_prefix(1);
        }
        if (text.empty() || !IsDigit(text.front()) || !IsDigit(text.back()))
            return false;
        for (size_t i = 0; i < text.size(); i++)
        {
            const char c = text[i];
            if (c == '_')
            {
                if (!IsDigit(text[i - 1]) || !IsDigit(text[i + 1]))
                    return false;
                continue;
            }
            if (!IsDigit(c))
                return false;
            if (value > (std::numeric_limits<long long>::max() - (c - '0')) / 10)
                overflow = true;
            else
                value = value * 10 + (c - '0');
        }
        if (negative)
            value = -value;
        return true;
    }
} // namespace w2b::py
